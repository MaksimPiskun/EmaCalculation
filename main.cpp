#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/chrono.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;

template <typename MessageHandler>
class CoinBaseConnector
{
public:
    CoinBaseConnector(MessageHandler &handler, size_t msg_total) : m_handler(handler), m_msg_total(msg_total) {}

    void subscribe()
    {
        try
        {
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            nlohmann::json subscribe_message;

            subscribe_message["type"] = "subscribe";
            subscribe_message["channels"] = nlohmann::json::array();
            subscribe_message["channels"][0]["name"] = "ticker";
            subscribe_message["channels"][0]["product_ids"] = nlohmann::json::array();
            subscribe_message["channels"][0]["product_ids"][0] = "ETH-USD";
            subscribe_message["timestamp"] = timestamp;

            std::string json_message = subscribe_message.dump();
            asio::io_context ioc;

            ssl::context ctx(ssl::context::sslv23);
            ctx.set_default_verify_paths();

            beast::websocket::stream<beast::ssl_stream<asio::ip::tcp::socket>> ws(ioc, ctx);
            asio::ip::tcp::resolver resolver(ioc);
            auto results = resolver.resolve("ws-feed.exchange.coinbase.com", "443");

            asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "ws-feed.exchange.coinbase.com"))
            {
                throw std::runtime_error("Failed to set SNI Host Name");
            }
            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake("ws-feed.exchange.coinbase.com", "/");
            ws.write(asio::buffer(json_message));
            size_t msg_number = 0;
            while (msg_number < m_msg_total)
            {
                try
                {
                    ws.read(m_buffer);
                    m_msg = beast::buffers_to_string(m_buffer.data());

                    m_handler.process(m_msg);
                    m_buffer.consume(m_buffer.size());
                    msg_number++;
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Error reading WebSocket message: " << e.what() << std::endl;
                    break;
                }
            }
            close(ws);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error parsing message at " << __FUNCTION__ << " line " << __LINE__ << ": " << e.what() << std::endl;
        }
    }

private:
    void close(beast::websocket::stream<beast::ssl_stream<asio::ip::tcp::socket>> &ws)
    {
        try
        {
            ws.close(beast::websocket::close_code::normal);
            ws.next_layer().next_layer().lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both);
            ws.next_layer().next_layer().lowest_layer().close();
            std::cout << "Connection closed gracefully." << std::endl;
        }
        catch (const std::exception &e)
        {
            //ignore
        }
    }

private:
    size_t m_msg_total = 0;
    beast::flat_buffer m_buffer;
    std::string m_msg;
    MessageHandler &m_handler;
};

class AlphaComputer
{
public:
    AlphaComputer(std::chrono::seconds tau)
    {
        size_t tauMicroseconds = std::chrono::microseconds(tau).count();
        m_delta2Alpha.reserve(tauMicroseconds);
        m_delta2Alpha[0] = 0;
        for (size_t delay = 1; delay < tauMicroseconds; delay++)
        {
            m_delta2Alpha[delay] = double(1) - std::exp(-double(delay) / tauMicroseconds);
        }
    }
    double getAlpha(size_t delay)
    {
        if (m_delta2Alpha.find(delay) == m_delta2Alpha.cend())
            return 0;
        return m_delta2Alpha[delay];
    }

private:
    std::unordered_map<std::size_t, double> m_delta2Alpha;
};

class MessageHandler
{
public:
    MessageHandler(std::chrono::seconds tau) : m_alphas(tau)
    {
    }
    ~MessageHandler()
    {
        m_csv_file.flush();
        m_csv_file.close();
    }
    void process(const std::string &msg)
    {
        m_ctx = {};
        try
        {
            auto j = nlohmann::json::parse(msg);
            const auto &type = j.at("type").get<std::string>();
            if (type != "ticker") [[unlikely]]
            {
                return;
            }

            m_time = j.at("time").get<std::string>();
            m_ctx.delta = calculateDeltaInMicroseconds();
            m_ctx.price = std::stod(j.at("price").get<std::string>());
            m_ctx.best_bid = std::stod(j.at("best_bid").get<std::string>());
            m_ctx.best_ask = std::stod(j.at("best_ask").get<std::string>());
            m_ctx.mid_price = (m_ctx.best_bid + m_ctx.best_ask) / 2;
            updateEMA();
            dumpJsonToCsv(j);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error parsing message at " << __FUNCTION__ << " line " << __LINE__ << ": " << e.what() << std::endl;
        }
    }

private:
    std::chrono::system_clock::time_point parseTimestampToTimePoint(const std::string &timestamp_str)
    {

        std::string time_str = timestamp_str;
        if (time_str.back() == 'Z')
        {
            time_str.pop_back();
        }

        std::replace(time_str.begin(), time_str.end(), 'T', ' ');
        std::tm tm = {};
        std::istringstream ss(time_str);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

        if (ss.fail())
        {
            throw std::runtime_error("Failed to parse timestamp");
        }

        auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        size_t dot_pos = time_str.find('.');
        if (dot_pos != std::string::npos)
        {
            int microseconds = std::stoi(time_str.substr(dot_pos + 1));
            tp += std::chrono::microseconds(microseconds);
        }

        return tp;
    }

    long long calculateDeltaInMicroseconds()
    {
        try
        {
            auto tp = parseTimestampToTimePoint(m_time);
            auto duration = tp - m_previousTimePoint;
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration);
            m_previousTimePoint = tp;
            return micros.count();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error parsing message at " << __FUNCTION__ << " line " << __LINE__ << ": " << e.what() << std::endl;
            return 0;
        }
    }

    void dumpJsonToCsv(const nlohmann::json &json_data)
    {
        if (!m_csv_file.is_open())
        {
            m_csv_file = std::ofstream("log.csv", std::ios::app);
        }

        if (m_csv_file.tellp() == 0)
        {
            bool first = true;
            for (auto &el : json_data.items())
            {
                if (!first)
                {
                    m_csv_file << ",";
                }
                m_csv_file << el.key();
                first = false;
            }
            m_csv_file << ",ema_price,ema_mid_price\n";
        }

        bool first = true;
        for (auto &el : json_data.items())
        {
            if (!first)
            {
                m_csv_file << ",";
            }

            try
            {
                if (el.value().is_string())
                {
                    m_csv_file << el.value().get<std::string>();
                }
                else if (el.key() == "sequence")
                {
                    m_csv_file << el.value().get<size_t>();
                }
                else if (el.key() == "trade_id")
                {
                    m_csv_file << el.value().get<size_t>();
                }
                else if (el.value().is_number())
                {
                    m_csv_file << el.value().get<double>();
                }
                else if (el.value().is_boolean())
                {
                    m_csv_file << el.value().get<bool>();
                }
                else
                {
                    m_csv_file << el.value();
                }
            }
            catch (const nlohmann::json::exception &e)
            {
                std::cerr << "Error parsing message at " << __FUNCTION__ << " line " << __LINE__ << ": " << e.what() << std::endl;
            }
            first = false;
        }

        m_csv_file << "," << ema_price << ',' << ema_mid_price << "\n";
    }
    void updateEMA()
    {
        if (m_ctx.delta == 0)
        {
            m_ctx.delta++;
        }
        double alpha = m_alphas.getAlpha(m_ctx.delta);
        ema_price += alpha * (m_ctx.price - ema_price);
        ema_mid_price += alpha * (m_ctx.mid_price - ema_mid_price);
    }

private:
    AlphaComputer m_alphas;
    std::string m_time;
    std::chrono::system_clock::time_point m_previousTimePoint;
    struct ParsingContext
    {
        double price = 0.0;
        double mid_price = 0.0;
        double best_bid = 0.0;
        double best_ask = 0.0;
        size_t delta = 0;
    } m_ctx;
    double ema_price = 0.0;
    double ema_mid_price = 0.0;
    std::ofstream m_csv_file;
};

int main()
{
    try
    {
        std::chrono::seconds window{5};
        MessageHandler handler(window);
        CoinBaseConnector connector(handler, 1000);
        connector.subscribe();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error parsing message at " << __FUNCTION__ << " line " << __LINE__ << ": " << e.what() << std::endl;
    }
    return 0;
}
