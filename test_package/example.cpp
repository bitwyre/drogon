#include "drogon/drogon.h"
#include <iostream>
#include <future>

using namespace drogon;

int main()
{
    trantor::Logger::setLogLevel(trantor::Logger::kTrace);
    {
        auto client = HttpClient::newHttpClient("http://www.baidu.com");
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/s");
        req->setParameter("wd", "weixin");

        client->sendRequest(
            req,
            [](ReqResult result, const HttpResponsePtr &response) {
                std::cout << "receive response!" << std::endl;
                // auto headers=response.
                std::cout << response->getBody() << std::endl;
                auto cookies = response->cookies();
                for (auto const &cookie : cookies)
                {
                    std::cout << cookie.first << "="
                                << cookie.second.value()
                                << ":domain=" << cookie.second.domain()
                                << std::endl;
                }
            });
    }

    app().run();
}