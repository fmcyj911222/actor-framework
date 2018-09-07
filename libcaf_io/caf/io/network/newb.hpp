/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#pragma once

#include "caf/config.hpp"

#include <cstdint>
#include <cstring>
#include <tuple>

#include "caf/actor_clock.hpp"
#include "caf/callback.hpp"
#include "caf/config.hpp"
#include "caf/detail/call_cfun.hpp"
#include "caf/detail/enum_to_string.hpp"
#include "caf/detail/socket_guard.hpp"
#include "caf/io/broker.hpp"
#include "caf/io/middleman.hpp"
#include "caf/io/network/default_multiplexer.hpp"
#include "caf/io/network/event_handler.hpp"
#include "caf/io/network/interfaces.hpp"
#include "caf/io/network/native_socket.hpp"
#include "caf/io/network/rw_state.hpp"
#include "caf/mixin/behavior_changer.hpp"
#include "caf/mixin/requester.hpp"
#include "caf/mixin/sender.hpp"
#include "caf/scheduled_actor.hpp"

namespace caf {
namespace io {
namespace network {

// -- forward declarations -----------------------------------------------------

template <class T>
struct protocol_policy;

template <class T>
struct newb;

} // namespace io
} // namespace network

// -- required to make newb an actor ------------------------------------------

template <class T>
class behavior_type_of<io::network::newb<T>> {
public:
  using type = behavior;
};

namespace io {
namespace network {

// -- aliases ------------------------------------------------------------------

using byte_buffer = std::vector<char>;
using header_writer = caf::callback<byte_buffer&>;

// -- newb base ----------------------------------------------------------------

struct newb_base : public network::event_handler {
  newb_base(default_multiplexer& dm, native_socket sockfd);

  virtual void start() = 0;

  virtual void stop() = 0;

  virtual void io_error(operation op, error err) = 0;

  virtual void start_reading() = 0;

  virtual void stop_reading() = 0;

  virtual void start_writing() = 0;

  virtual void stop_writing() = 0;

};

// -- transport policy ---------------------------------------------------------

struct transport_policy {
  transport_policy();

  virtual ~transport_policy();

  virtual io::network::rw_state write_some(newb_base*);

  virtual io::network::rw_state read_some(newb_base*);

  virtual bool should_deliver();

  virtual bool must_read_more(newb_base*);

  virtual void prepare_next_read(newb_base*);

  virtual void prepare_next_write(newb_base*);

  virtual void configure_read(receive_policy::config);

  virtual void flush(newb_base*);

  virtual byte_buffer& wr_buf();

  template <class T>
  error read_some(newb_base* parent, protocol_policy<T>& policy) {
    CAF_LOG_TRACE("");
    size_t reads = 0;
    while (reads < max_consecutive_reads || must_read_more(parent)) {
      auto read_result = read_some(parent);
      switch (read_result) {
        case rw_state::success:
          if (received_bytes == 0)
            return none;
          if (should_deliver()) {
            auto res = policy.read(receive_buffer.data(), received_bytes);
            prepare_next_read(parent);
            if (res)
              return res;
          }
          break;
        case rw_state::indeterminate:
          // No error, but don't continue reading.
          return none;
        case rw_state::failure:
          // Reading failed.
          return sec::runtime_error;
      }
      ++reads;
    }
    return none;
  }

  virtual expected<native_socket>
  connect(const std::string&, uint16_t,
          optional<io::network::protocol::network> = none);

  size_t received_bytes;
  size_t max_consecutive_reads;

  byte_buffer offline_buffer;
  byte_buffer receive_buffer;
  byte_buffer send_buffer;
};

using transport_policy_ptr = std::unique_ptr<transport_policy>;

// -- accept policy ------------------------------------------------------------

struct accept_policy {
  virtual ~accept_policy();

  virtual expected<native_socket> create_socket(uint16_t port, const char* host,
                                                bool reuse = false) = 0;

  virtual std::pair<native_socket, transport_policy_ptr> accept(newb_base*) = 0;

  virtual error multiplex_write() {
    return none;
  }

  virtual void init(newb_base&) = 0;
};

// -- protocol policy ----------------------------------------------------------

struct protocol_policy_base {
  virtual ~protocol_policy_base();

  virtual error read(char* bytes, size_t count) = 0;

  virtual error timeout(atom_value, uint32_t) = 0;

  virtual void write_header(byte_buffer&, header_writer*) = 0;

  virtual void prepare_for_sending(byte_buffer&, size_t, size_t, size_t) = 0;
};

template <class T>
struct protocol_policy : protocol_policy_base {
  using message_type = T;
  virtual ~protocol_policy() override {
    // nop
  }
};

template <class T>
using protocol_policy_ptr = std::unique_ptr<protocol_policy<T>>;

template <class T>
struct generic_protocol
    : public io::network::protocol_policy<typename T::message_type> {
  T impl;

  generic_protocol(io::network::newb<typename T::message_type>* parent)
      : impl(parent) {
    // nop
  }

  error read(char* bytes, size_t count) override {
    return impl.read(bytes, count);
  }

  error timeout(atom_value atm, uint32_t id) override {
    return impl.timeout(atm, id);
  }

  void write_header(io::network::byte_buffer& buf,
                    io::network::header_writer* hw) override {
    impl.write_header(buf, hw);
  }

  void prepare_for_sending(io::network::byte_buffer& buf, size_t hstart,
                           size_t offset, size_t plen) override {
    impl.prepare_for_sending(buf, hstart, offset, plen);
  }
};

// -- new broker classes -------------------------------------------------------

/// @relates newb
/// Returned by funtion wr_buf of newb.
template <class Message>
struct write_handle {
  newb<Message>* parent;
  protocol_policy_base* protocol;
  byte_buffer* buf;
  size_t header_start;
  size_t header_len;

  ~write_handle() {
    // Can we calculate added bytes for datagram things?
    auto payload_size = buf->size() - (header_start + header_len);
    protocol->prepare_for_sending(*buf, header_start, 0, payload_size);
    parent->flush();
  }
};

template <class Message>
struct newb : public extend<scheduled_actor, newb<Message>>::template
                     with<mixin::sender, mixin::requester,
                          mixin::behavior_changer>,
              public dynamically_typed_actor_base,
              public newb_base {
  using super = typename extend<scheduled_actor, newb<Message>>::
    template with<mixin::sender, mixin::requester, mixin::behavior_changer>;

  using signatures = none_t;

  using message_type = Message;

  // -- constructors and destructors -------------------------------------------

  newb(actor_config& cfg, default_multiplexer& dm, native_socket sockfd)
      : super(cfg),
        newb_base(dm, sockfd),
        value_(strong_actor_ptr{}, make_message_id(),
               mailbox_element::forwarding_stack{}, Message{}){
    CAF_LOG_TRACE("");
    scheduled_actor::set_timeout_handler([&](timeout_msg& msg) {
      if (protocol)
        protocol->timeout(msg.type, msg.timeout_id);
    });
  }

  newb() = default;

  newb(newb<Message>&&) = default;

  ~newb() override {
    CAF_LOG_TRACE("");
  }

  // -- overridden modifiers of abstract_actor ---------------------------------

  void enqueue(mailbox_element_ptr ptr, execution_unit*) override {
    CAF_PUSH_AID(this->id());
    scheduled_actor::enqueue(std::move(ptr), &backend());
  }

  void enqueue(strong_actor_ptr src, message_id mid, message msg,
               execution_unit*) override {
    enqueue(make_mailbox_element(std::move(src), mid, {}, std::move(msg)),
            &backend());
  }

  resumable::subtype_t subtype() const override {
    return resumable::io_actor;
  }

  const char* name() const override {
    return "newb";
  }

  // -- overridden modifiers of local_actor ------------------------------------

  void launch(execution_unit* eu, bool lazy, bool hide) override {
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_ASSERT(eu != nullptr);
    CAF_ASSERT(eu == &backend());
    CAF_LOG_TRACE(CAF_ARG(lazy) << CAF_ARG(hide));
    // Add implicit reference count held by middleman/multiplexer.
    if (!hide)
      super::register_at_system();
    if (lazy && super::mailbox().try_block())
      return;
    intrusive_ptr_add_ref(super::ctrl());
    eu->exec_later(this);
  }

  void initialize() override {
    CAF_LOG_TRACE("");
    init_newb();
    auto bhvr = make_behavior();
    CAF_LOG_DEBUG_IF(!bhvr, "make_behavior() did not return a behavior:"
                             << CAF_ARG(this->has_behavior()));
    if (bhvr) {
      // make_behavior() did return a behavior instead of using become().
      CAF_LOG_DEBUG("make_behavior() did return a valid behavior");
      this->become(std::move(bhvr));
    }
  }

  bool cleanup(error&& reason, execution_unit* host) override {
    CAF_LOG_TRACE(CAF_ARG(reason));
    stop(); // ???
    // TODO: Should policies be notified here?
    return local_actor::cleanup(std::move(reason), host);
  }

  // -- overridden modifiers of resumable --------------------------------------

  network::multiplexer::runnable::resume_result resume(execution_unit* ctx,
                                                       size_t mt) override {
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_ASSERT(ctx != nullptr);
    CAF_ASSERT(ctx == &backend());
    return scheduled_actor::resume(ctx, mt);
  }

  // -- overridden modifiers of event handler ----------------------------------

  void handle_event(network::operation op) override {
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_LOG_TRACE("");
    switch (op) {
      case io::network::operation::read:
        read_event();
        break;
      case io::network::operation::write:
        write_event();
        break;
      case io::network::operation::propagate_error:
        handle_error();
    }
  }

  void removed_from_loop(network::operation op) override {
    CAF_LOG_DEBUG("newb removed from loop: " << to_string(op));
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_LOG_TRACE(CAF_ARG(op));
    switch (op) {
      case network::operation::read:
        reading_ = false;
        break;
      case network::operation::write:
        writing_ = false;
        break;
      case network::operation::propagate_error: ; // nop
    }
    // Quit if there is nothing left to do.
    if (!reading_ && !writing_)
      intrusive_ptr_release(super::ctrl());
  }

  // -- base requirements ------------------------------------------------------

  void start() override {
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_LOG_TRACE("");
    intrusive_ptr_add_ref(super::ctrl());
    CAF_LOG_DEBUG("starting newb");
    start_reading();
    if (transport)
      transport->prepare_next_read(this);
  }

  void stop() override {
    CAF_PUSH_AID_FROM_PTR(this);
    CAF_LOG_TRACE("");
    close_read_channel();
    stop_reading();
    stop_writing();
  }

  void io_error(operation op, error err) override {
    // TODO: This message is not always handled correctly.
    //  Don't know why yet ...
    auto mptr = make_mailbox_element(nullptr, invalid_message_id, {},
                                     io_error_msg{op, std::move(err)});
    switch (scheduled_actor::consume(*mptr)) {
      case im_success:
        this->finalize();
        break;
      case im_skipped:
        this->push_to_cache(std::move(mptr));
        break;
      case im_dropped:
        CAF_LOG_INFO("broker dropped read error message");
        break;
    }
    switch (op) {
      case operation::read:
        stop_reading();
        break;
      case operation::write:
        stop_writing();
        break;
      case operation::propagate_error:
        // TODO: What should happen here?
        break;
    }
  }

  void start_reading() override {
    if (!reading_) {
      event_handler::activate();
      reading_ = true;
    }
  }

  void stop_reading() override {
    passivate();
  }

  void start_writing() override {
    if (!writing_) {
      event_handler::backend().add(operation::write, fd(), this);
      writing_ = true;
    }
  }

  void stop_writing() override {
    event_handler::backend().del(operation::write, fd(), this);
  }

  // -- members ----------------------------------------------------------------

  void init_newb() {
    CAF_LOG_TRACE("");
    super::setf(super::is_initialized_flag);
  }

  /// Get a write buffer to write data to be sent by this broker.
  write_handle<Message> wr_buf(header_writer* hw) {
    auto& buf = transport->wr_buf();
    auto hstart = buf.size();
    protocol->write_header(buf, hw);
    auto hlen = buf.size() - hstart;
    return {this, protocol.get(), &buf, hstart, hlen};
  }

  byte_buffer& wr_buf() {
    return transport->wr_buf();
  }

  void flush() {
    transport->flush(this);
  }

  void read_event() {
    auto err = transport->read_some(this, *protocol);
    if (err)
      io_error(operation::read, std::move(err));
  }

  void write_event() {
    if (transport->write_some(this) == rw_state::failure)
      io_error(operation::write, sec::runtime_error);
  }

  void handle_error() {
    CAF_CRITICAL("got error to handle: not implemented");
  }

  /// Set a timeout for a protocol policy layer.
  template<class Rep = int, class Period = std::ratio<1>>
  void set_timeout(std::chrono::duration<Rep, Period> timeout,
                   atom_value atm, uint32_t id) {
    auto n = actor_clock::clock_type::now();
    scheduled_actor::clock().set_ordinary_timeout(n + timeout, this, atm, id);
  }

  /// Returns the `multiplexer` running this broker.
  network::multiplexer& backend() {
    return event_handler::backend();
  }

  /// Pass a message from a protocol policy layer to the broker for processing.
  void handle(Message& m) {
    std::swap(msg(), m);
    scheduled_actor::activate(scheduled_actor::context(), value_);
  }

  /// Override this to set the behavior of the broker.
  virtual behavior make_behavior() {
    behavior res;
    if (this->initial_behavior_fac_) {
      res = this->initial_behavior_fac_(this);
      this->initial_behavior_fac_ = nullptr;
    }
    return res;
  }

  /// Configure the number of bytes read for the next packet. (Can be ignored by
  /// the transport policy if its protocol does not support this functionality.)
  void configure_read(receive_policy::config config) {
    transport->configure_read(config);
  }

  /// @cond PRIVATE

  template <class... Ts>
  void eq_impl(message_id mid, strong_actor_ptr sender,
               execution_unit* ctx, Ts&&... xs) {
    enqueue(make_mailbox_element(std::move(sender), mid,
                                 {}, std::forward<Ts>(xs)...),
            ctx);
  }

  // -- policies ---------------------------------------------------------------

  std::unique_ptr<transport_policy> transport;
  std::unique_ptr<protocol_policy<Message>> protocol;

  /// @endcond

private:
  // -- message element --------------------------------------------------------

  Message& msg() {
    return value_.template get_mutable_as<Message>(0);
  }

  mailbox_element_vals<Message> value_;
  bool reading_;
  bool writing_;
};

// -- new broker acceptor ------------------------------------------------------

template <class Message>
struct newb_acceptor : public newb_base, public caf::ref_counted {

  // -- constructors and destructors -------------------------------------------

  newb_acceptor(default_multiplexer& dm, native_socket sockfd)
      : newb_base(dm, sockfd) {
    // nop
  }

  ~newb_acceptor() {
    // nop
  }

  // -- overridden modifiers of event handler ----------------------------------

  void handle_event(network::operation op) override {
    CAF_LOG_DEBUG("new event: " << to_string(op));
    switch (op) {
      case network::operation::read:
        read_event();
        break;
      case network::operation::write:
        // Required to multiplex over a single socket.
        write_event();
        break;
      case network::operation::propagate_error:
        CAF_LOG_DEBUG("acceptor got error operation");
        break;
    }
  }

  void removed_from_loop(network::operation op) override {
    CAF_LOG_TRACE(CAF_ARG(op));
    CAF_LOG_DEBUG("newb removed from loop: " << to_string(op));
    switch (op) {
      case network::operation::read:
        reading_ = false;
        break;
      case network::operation::write:
        writing_ = false;
        break;
      case network::operation::propagate_error: ; // nop
    }
    // Quit if there is nothing left to do.
    if (!reading_ && !writing_)
      deref();
  }

  // -- base requirements ------------------------------------------------------

  void start() override {
    start_reading();
    ref();
  }

  void stop() override {
    CAF_LOG_TRACE(CAF_ARG2("fd", fd()));
    close_read_channel();
    stop_reading();
    stop_writing();
  }

  void io_error(operation op, error err) override {
    std::cerr << "operation " << to_string(op) << " failed: "
              << backend().system().render(err) << std::endl;
    stop();
  }

  void start_reading() override {
    if (!reading_) {
      activate();
      reading_ = true;
    }
  }

  void stop_reading() override {
    passivate();
  }

  void start_writing() override {
    if (!writing_) {
      event_handler::backend().add(operation::write, fd(), this);
      writing_ = true;
    }
  }

  void stop_writing() override {
    event_handler::backend().del(operation::write, fd(), this);
  }

  // -- members ----------------------------------------------------------------

  void read_event() {
    native_socket sock;
    transport_policy_ptr transport;
    std::tie(sock, transport) = acceptor->accept(this);
    auto en = create_newb(sock, std::move(transport));
    if (!en) {
      io_error(operation::read, std::move(en.error()));
      return;
    }
    auto ptr = caf::actor_cast<caf::abstract_actor*>(*en);
    CAF_ASSERT(ptr != nullptr);
    auto& ref = dynamic_cast<newb<Message>&>(*ptr);
    acceptor->init(ref);
  }

  void write_event() {
    acceptor->multiplex_write();
  }

  virtual expected<actor> create_newb(native_socket sock,
                                      transport_policy_ptr pol) = 0;

  std::unique_ptr<accept_policy> acceptor;

private:
  bool reading_;
  bool writing_;
};

template <class T>
using acceptor_ptr = caf::intrusive_ptr<newb_acceptor<T>>;

// -- factories ----------------------------------------------------------------

// Goal:
// spawn_newb<protocol>(behavior, transport, sockfd);
// spawn_client<protocol>(behavior, transport, host, port);
// spawn_server<protocol>(behavior, transport, port);
// TODO: Should the server be an actor as well? Would give us a place to handle
//  its failures and the previously existing `new_endpoint_msg`.

// Primary template.
template<class T>
struct function_traits : function_traits<decltype(&T::operator())> {
};

// Partial specialization for function type.
template<class R, class... Args>
struct function_traits<R(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<Args...>;
};

// Partial specialization for function pointer.
template<class R, class... Args>
struct function_traits<R (*)(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<Args...>;
};

// Partial specialization for std::function.
template<class R, class... Args>
struct function_traits<std::function<R(Args...)>> {
    using result_type = R;
    using argument_types = std::tuple<Args...>;
};

// Partial specialization for pointer-to-member-function (i.e., operator()'s).
template<class T, class R, class... Args>
struct function_traits<R (T::*)(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<Args...>;
};

template<class T, class R, class... Args>
struct function_traits<R (T::*)(Args...) const> {
    using result_type = R;
    using argument_types = std::tuple<Args...>;
};

template<class T>
using first_argument_type
 = typename std::tuple_element<0, typename function_traits<T>::argument_types>::type;

/// Spawns a new "newb" broker.
template <class Protocol, spawn_options Os = no_spawn_options, class F, class... Ts>
typename infer_handle_from_fun<F>::type
spawn_newb(actor_system& sys, F fun, transport_policy_ptr transport,
           native_socket sockfd, Ts&&... xs) {
  using impl = typename infer_handle_from_fun<F>::impl;
  using first = first_argument_type<F>;
  using message = typename std::remove_pointer<first>::type::message_type;
  auto& dm = dynamic_cast<default_multiplexer&>(sys.middleman().backend());
  // Setup the config.
  actor_config cfg{&dm};
  detail::init_fun_factory<impl, F> fac;
  auto init_fun = fac(std::move(fun), std::forward<Ts>(xs)...);
  cfg.init_fun = [init_fun](local_actor* self) mutable -> behavior {
    return init_fun(self);
  };
  auto res = sys.spawn_class<impl, Os>(cfg, dm, sockfd);
  // Get a reference to the newb type.
  auto ptr = caf::actor_cast<caf::abstract_actor*>(res);
  CAF_ASSERT(ptr != nullptr);
  auto& ref = dynamic_cast<newb<message>&>(*ptr);
  // Set the policies.
  ref.transport = std::move(transport);
  ref.protocol.reset(new Protocol(&ref));
  // Start the event handler.
  ref.start();
  return res;
}

/// Spawn a new "newb" broker client to connect to `host`:`port`.
template <class Protocol, spawn_options Os = no_spawn_options, class F, class... Ts>
expected<typename infer_handle_from_fun<F>::type>
spawn_client(actor_system& sys, F fun, transport_policy_ptr transport,
             std::string host, uint16_t port, Ts&&... xs) {
  expected<native_socket> esock = transport->connect(host, port);
  if (!esock)
    return std::move(esock.error());
  return spawn_newb<Protocol>(sys, fun, std::move(transport), *esock,
                              std::forward<Ts>(xs)...);
}

template <class Newb>
actor make_newb(actor_system& sys, native_socket sockfd) {
  auto& mpx = dynamic_cast<default_multiplexer&>(sys.middleman().backend());
  actor_config acfg{&mpx};
  auto res = sys.spawn_impl<Newb, hidden + lazy_init>(acfg, mpx, sockfd);
  return actor_cast<actor>(res);
}

template <class Newb, class Transport, class Protocol>
actor make_client_newb(actor_system& sys, std::string host, uint16_t port) {
  transport_policy_ptr trans{new Transport};
  expected<native_socket> esock = trans->connect(host, port);
  if (!esock)
    return {};
  auto res = make_newb<Newb>(sys, *esock);
  auto ptr = caf::actor_cast<caf::abstract_actor*>(res);
  CAF_ASSERT(ptr != nullptr);
  auto& ref = dynamic_cast<Newb&>(*ptr);
  ref.transport = std::move(trans);
  ref.protocol.reset(new Protocol(&ref));
  ref.start();
  return res;
}

template <class NewbAcceptor, class AcceptPolicy>
caf::intrusive_ptr<NewbAcceptor> make_server_newb(actor_system& sys,
                                                  uint16_t port,
                                                  const char* addr = nullptr,
                                                  bool reuse_addr = false) {
  std::unique_ptr<AcceptPolicy> acc{new AcceptPolicy};
  auto esock = acc->create_socket(port, addr, reuse_addr);
  // new_tcp_acceptor_impl(port, addr, reuse_addr);
  if (!esock) {
    CAF_LOG_DEBUG("Could not open " << CAF_ARG(port) << CAF_ARG(addr));
    return nullptr;
  }
  auto& mpx = dynamic_cast<default_multiplexer&>(sys.middleman().backend());
  auto ptr = caf::make_counted<NewbAcceptor>(mpx, *esock);
  ptr->acceptor = std::move(acc);
  ptr->start();
  return ptr;
}

/// Convenience template alias for declaring state-based brokers.
template <class Message, class State>
using stateful_newb = stateful_actor<State, newb<Message>>;

} // namespace network
} // namespace io
} // namespace caf

