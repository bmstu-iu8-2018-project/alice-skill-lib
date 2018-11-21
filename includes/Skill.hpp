#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>
#include <boost/system/error_code.hpp>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

template <class Stream>
struct send_lambda {
  Stream& stream_;
  bool& close_;
  boost::system::error_code& ec_;
  
  explicit send_lambda(Stream& stream,
                       bool& close,
                       boost::system::error_code& ec)
      : stream_(stream), close_(close), ec_(ec) {}

  template <bool isRequest, class Body, class Fields>
  void operator()(http::message<isRequest, Body, Fields>&& msg) const {
    close_ = msg.need_eof();
    http::serializer<isRequest, Body, Fields> sr{msg};
    http::write(stream_, sr, ec_);
  }
};

struct Skill {
  Skill() {}
  
  template <class Body, class Allocator, class Send>
  void handle_request(http::request<Body, http::basic_fields<Allocator>>&& req,
                      Send&& send) {
    Alice::Request alice_request(req.body());
    Alice::Response alice_response(alice_request);
    callback_(alice_request, alice_response);
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.body() = alice_response.ToString();
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    res.prepare_payload();
    return send(std::move(res));
  }

  void set_callback(std::function<void(const Alice::Request& request,
                                      Alice::Response& response)> callback) {
    callback_ = std::move(callback);
  }

  void run() {
    auto const address = boost::asio::ip::make_address("0.0.0.0");
    auto const port_arg = get_os_env("PORT", "5000");
    auto const port = static_cast<unsigned short>(std::atoi(port_arg.c_str()));
    std::shared_ptr<std::string const> const doc_root_{
        std::make_shared<std::string>(".")};
    boost::asio::io_context ioc_{1};
    tcp::acceptor acceptor_{ioc_, {address, port}};
    for (;;) {
      tcp::socket socket_{ioc_};
      acceptor_.accept(socket_);
      std::thread{&Skill::do_session_, this, std::move(socket_), doc_root_}
          .detach();
    }
  }

 private:
  std::function<void(const Alice::Request& request, Alice::Response& response)>
      callback_;

  void do_session_(tcp::socket&& socket,
                  std::shared_ptr<std::string const> const& doc_root) {
    bool close = false;
    boost::system::error_code ec;
    boost::beast::flat_buffer buffer;
    send_lambda<tcp::socket> lambda{socket, close, ec};
    for (;;) {
      http::request<http::string_body> req;
      http::read(socket, buffer, req, ec);
      if (ec == http::error::end_of_stream)
        break;
      if (ec)
        return fail(ec, "read");
      handle_request(std::move(req), lambda);
      if (ec)
        return fail(ec, "write");
      if (close) {
        break;
      }
    }
    socket.shutdown(tcp::socket::shutdown_send, ec);
  }

  void fail(boost::system::error_code ec, char const* what) {
    std::cerr << what << ": " << ec.message() << "\n";
  }

  std::string get_os_env(boost::string_view name,
                       boost::string_view default_value) {
    const char* e = std::getenv(name.data());
    return e ? e : default_value.data();
  }
};