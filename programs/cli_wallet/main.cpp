/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <iterator>

#include <boost/algorithm/string/replace.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/version.hpp>

#include <fc/interprocess/signals.hpp>
#include <fc/io/json.hpp>
#include <fc/io/stdio.hpp>
#include <fc/log/console_appender.hpp>
#include <fc/log/file_appender.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/network/http/server.hpp>
#include <fc/network/http/websocket.hpp>
#include <fc/rpc/cli.hpp>
#include <fc/rpc/http_api.hpp>
#include <fc/rpc/websocket_api.hpp>
#include <fc/smart_ref_impl.hpp>

#include <graphene/app/api.hpp>
#include <graphene/chain/config.hpp>
#include <graphene/chain/protocol/protocol.hpp>
#include <graphene/egenesis/egenesis.hpp>
#include <graphene/utilities/git_revision.hpp>
#include <graphene/utilities/key_conversion.hpp>
#include <graphene/wallet/wallet.hpp>

#ifdef WIN32
#include <signal.h>
#else
#include <csignal>
#endif

using namespace graphene::app;
using namespace graphene::chain;
using namespace graphene::utilities;
using namespace graphene::wallet;
using namespace std;
namespace bpo = boost::program_options;

int main(int argc, char **argv) {
   try {

      boost::program_options::options_description opts;
      opts.add_options()("help,h", "Print this help message and exit.");
      opts.add_options()("version,v", "Display the version info and exit");
      opts.add_options()("server-rpc-endpoint,s", bpo::value<string>()->implicit_value("ws://127.0.0.1:8090"), "Server websocket RPC endpoint");
      opts.add_options()("server-rpc-user,u", bpo::value<string>(), "Server Username");
      opts.add_options()("server-rpc-password,p", bpo::value<string>(), "Server Password");
      opts.add_options()("rpc-endpoint,r", bpo::value<string>()->implicit_value("127.0.0.1:8091"), "Endpoint for wallet websocket RPC to listen on");
      opts.add_options()("rpc-tls-endpoint,t", bpo::value<string>()->implicit_value("127.0.0.1:8092"), "Endpoint for wallet websocket TLS RPC to listen on");
      opts.add_options()("rpc-tls-certificate,c", bpo::value<string>()->implicit_value("server.pem"), "PEM certificate for wallet websocket TLS RPC");
      opts.add_options()("rpc-http-endpoint,H", bpo::value<string>()->implicit_value("127.0.0.1:8093"), "Endpoint for wallet HTTP RPC to listen on");
      opts.add_options()("daemon,d", "Run the wallet in daemon mode");
      opts.add_options()("wallet-file,w", bpo::value<string>()->implicit_value("wallet.json"), "wallet to load");
      opts.add_options()("chain-id", bpo::value<string>(), "chain ID to connect to");

      bpo::variables_map options;

      try {
         bpo::parsed_options po = bpo::command_line_parser(argc, argv).options(opts).allow_unregistered().run();
         std::vector<std::string> unrecognized = bpo::collect_unrecognized(po.options, bpo::include_positional);
         if (unrecognized.size() > 0) {
            std::cout << "Unknown parameter(s): " << std::endl;
            for (auto s : unrecognized) {
               std::cout << "  " << s << std::endl;
            }
            return 0;
         }
         bpo::store(po, options);
      } catch (const boost::program_options::invalid_command_line_syntax &e) {
         std::cout << e.what() << std::endl;
         return 0;
      }

      if (options.count("help")) {
         std::cout << opts << "\n";
         return 0;
      }

      if (options.count("version")) {
         std::string wallet_version(graphene::utilities::git_revision_description);
         const size_t pos = wallet_version.find('/');
         if (pos != std::string::npos && wallet_version.size() > pos)
            wallet_version = wallet_version.substr(pos + 1);
         std::cout << "Version: " << wallet_version << "\n";
         std::cout << "Git Revision: " << graphene::utilities::git_revision_sha << "\n";
         std::cout << "Built: " << __DATE__ " at " __TIME__ << "\n";
         std::cout << "SSL: " << OPENSSL_VERSION_TEXT << "\n";
         std::cout << "Boost: " << boost::replace_all_copy(std::string(BOOST_LIB_VERSION), "_", ".") << "\n";
         return 0;
      }

      fc::path data_dir;
      fc::logging_config cfg;
      fc::path log_dir = data_dir / "logs";

      fc::file_appender::config ac;
      ac.filename = log_dir / "rpc" / "rpc.log";
      ac.flush = true;
      ac.rotate = true;
      ac.rotation_interval = fc::hours(1);
      ac.rotation_limit = fc::days(1);

      std::cout << "Logging RPC to file: " << (data_dir / ac.filename).preferred_string() << "\n";

      cfg.appenders.push_back(fc::appender_config("default", "console", fc::variant(fc::console_appender::config(), 20)));
      cfg.appenders.push_back(fc::appender_config("rpc", "file", fc::variant(ac, 5)));

      cfg.loggers = {fc::logger_config("default"), fc::logger_config("rpc")};
      cfg.loggers.front().level = fc::log_level::warn;
      cfg.loggers.front().appenders = {"default"};
      cfg.loggers.back().level = fc::log_level::info;
      cfg.loggers.back().appenders = {"rpc"};

      fc::configure_logging(cfg);

      fc::ecc::private_key committee_private_key = fc::ecc::private_key::regenerate(fc::sha256::hash(string("null_key")));

      idump((key_to_wif(committee_private_key)));

      fc::ecc::private_key nathan_private_key = fc::ecc::private_key::regenerate(fc::sha256::hash(string("nathan")));
      public_key_type nathan_pub_key = nathan_private_key.get_public_key();
      idump((nathan_pub_key));
      idump((key_to_wif(nathan_private_key)));

      //
      // TODO:  We read wallet_data twice, once in main() to grab the
      //    socket info, again in wallet_api when we do
      //    load_wallet_file().  Seems like this could be better
      //    designed.
      //
      wallet_data wdata;

      fc::path wallet_file(options.count("wallet-file") ? options.at("wallet-file").as<string>() : "wallet.json");
      if (fc::exists(wallet_file)) {
         wdata = fc::json::from_file(wallet_file).as<wallet_data>(GRAPHENE_MAX_NESTED_OBJECTS);
         if (options.count("chain-id")) {
            // the --chain-id on the CLI must match the chain ID embedded in the wallet file
            if (chain_id_type(options.at("chain-id").as<std::string>()) != wdata.chain_id) {
               std::cout << "Chain ID in wallet file does not match specified chain ID\n";
               return 1;
            }
         }
      } else {
         if (options.count("chain-id")) {
            wdata.chain_id = chain_id_type(options.at("chain-id").as<std::string>());
            std::cout << "Starting a new wallet with chain ID " << wdata.chain_id.str() << " (from CLI)\n";
         } else {
            wdata.chain_id = graphene::egenesis::get_egenesis_chain_id();
            std::cout << "Starting a new wallet with chain ID " << wdata.chain_id.str() << " (from egenesis)\n";
         }
      }

      // but allow CLI to override
      if (options.count("server-rpc-endpoint"))
         wdata.ws_server = options.at("server-rpc-endpoint").as<std::string>();
      if (options.count("server-rpc-user"))
         wdata.ws_user = options.at("server-rpc-user").as<std::string>();
      if (options.count("server-rpc-password"))
         wdata.ws_password = options.at("server-rpc-password").as<std::string>();

      fc::http::websocket_client client;
      idump((wdata.ws_server));
      auto con = client.connect(wdata.ws_server);
      auto apic = std::make_shared<fc::rpc::websocket_api_connection>(con, GRAPHENE_MAX_NESTED_OBJECTS);

      auto remote_api = apic->get_remote_api<login_api>(1);
      edump((wdata.ws_user)(wdata.ws_password));
      FC_ASSERT(remote_api->login(wdata.ws_user, wdata.ws_password), "Failed to log in to API server");

      auto wapiptr = std::make_shared<wallet_api>(wdata, remote_api);
      wapiptr->set_wallet_filename(wallet_file.generic_string());
      wapiptr->load_wallet_file();

      fc::api<wallet_api> wapi(wapiptr);

      auto wallet_cli = std::make_shared<fc::rpc::cli>(GRAPHENE_MAX_NESTED_OBJECTS);
      for (auto &name_formatter : wapiptr->get_result_formatters())
         wallet_cli->format_result(name_formatter.first, name_formatter.second);

      boost::signals2::scoped_connection closed_connection(con->closed.connect([wallet_cli] {
         cerr << "Server has disconnected us.\n";
         wallet_cli->stop();
      }));
      (void)(closed_connection);

      if (wapiptr->is_new()) {
         std::cout << "Please use the set_password method to initialize a new wallet before continuing\n";
         wallet_cli->set_prompt("new >>> ");
      } else
         wallet_cli->set_prompt("locked >>> ");

      boost::signals2::scoped_connection locked_connection(wapiptr->lock_changed.connect([&](bool locked) {
         wallet_cli->set_prompt(locked ? "locked >>> " : "unlocked >>> ");
      }));

      std::shared_ptr<fc::http::websocket_server> _websocket_server;
      if (options.count("rpc-endpoint")) {
         _websocket_server = std::make_shared<fc::http::websocket_server>();
         _websocket_server->on_connection([&wapi](const fc::http::websocket_connection_ptr &c) {
            std::cout << "here... \n";
            wlog(".");
            auto wsc = std::make_shared<fc::rpc::websocket_api_connection>(c, GRAPHENE_MAX_NESTED_OBJECTS);
            wsc->register_api(wapi);
            c->set_session_data(wsc);
         });
         ilog("Listening for incoming RPC requests on ${p}", ("p", options.at("rpc-endpoint").as<string>()));
         _websocket_server->listen(fc::ip::endpoint::from_string(options.at("rpc-endpoint").as<string>()));
         _websocket_server->start_accept();
      }

      string cert_pem = "server.pem";
      if (options.count("rpc-tls-certificate"))
         cert_pem = options.at("rpc-tls-certificate").as<string>();

      std::shared_ptr<fc::http::websocket_tls_server> _websocket_tls_server;
      if (options.count("rpc-tls-endpoint")) {
         _websocket_tls_server = std::make_shared<fc::http::websocket_tls_server>(cert_pem);
         _websocket_tls_server->on_connection([&](const fc::http::websocket_connection_ptr &c) {
            auto wsc = std::make_shared<fc::rpc::websocket_api_connection>(c, GRAPHENE_MAX_NESTED_OBJECTS);
            wsc->register_api(wapi);
            c->set_session_data(wsc);
         });
         ilog("Listening for incoming TLS RPC requests on ${p}", ("p", options.at("rpc-tls-endpoint").as<string>()));
         _websocket_tls_server->listen(fc::ip::endpoint::from_string(options.at("rpc-tls-endpoint").as<string>()));
         _websocket_tls_server->start_accept();
      }

      auto _http_server = std::make_shared<fc::http::server>();
      if (options.count("rpc-http-endpoint")) {
         ilog("Listening for incoming HTTP RPC requests on ${p}", ("p", options.at("rpc-http-endpoint").as<string>()));
         _http_server->listen(fc::ip::endpoint::from_string(options.at("rpc-http-endpoint").as<string>()));
         //
         // due to implementation, on_request() must come AFTER listen()
         //
         _http_server->on_request(
               [&wapi](const fc::http::request &req, const fc::http::server::response &resp) {
                  std::shared_ptr<fc::rpc::http_api_connection> conn =
                        std::make_shared<fc::rpc::http_api_connection>(GRAPHENE_MAX_NESTED_OBJECTS);
                  conn->register_api(wapi);
                  conn->on_request(req, resp);
               });
      }

      if (!options.count("daemon")) {
         wallet_cli->register_api(wapi);
         wallet_cli->start();
         wallet_cli->wait();
      } else {
         fc::promise<int>::ptr exit_promise = new fc::promise<int>("UNIX Signal Handler");
         fc::set_signal_handler([&exit_promise](int signal) {
            exit_promise->set_value(signal);
         },
                                SIGINT);

         ilog("Entering Daemon Mode, ^C to exit");
         exit_promise->wait();
      }

      wapi->save_wallet_file(wallet_file.generic_string());
      locked_connection.disconnect();
      closed_connection.disconnect();
   } catch (const fc::exception &e) {
      std::cout << e.to_detail_string() << "\n";
      return -1;
   }
   return 0;
}
