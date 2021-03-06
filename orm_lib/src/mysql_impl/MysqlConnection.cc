/**
 *
 *  MysqlConnection.cc
 *  An Tao
 *
 *  Copyright 2018, An Tao.  All rights reserved.
 *  https://github.com/an-tao/drogon
 *  Use of this source code is governed by a MIT license
 *  that can be found in the License file.
 *
 *  Drogon
 *
 */

#include "MysqlConnection.h"
#include "MysqlResultImpl.h"
#include <algorithm>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/string_view.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <regex>

using namespace drogon;
using namespace drogon::orm;
namespace drogon
{
namespace orm
{
Result makeResult(
    const std::shared_ptr<MYSQL_RES> &r = std::shared_ptr<MYSQL_RES>(nullptr),
    Result::SizeType affectedRows = 0,
    unsigned long long insertId = 0)
{
    return Result(std::shared_ptr<MysqlResultImpl>(
        new MysqlResultImpl(r, affectedRows, insertId)));
}

}  // namespace orm
}  // namespace drogon

MysqlConnection::MysqlConnection(trantor::EventLoop *loop,
                                 const std::string &connInfo)
    : DbConnection(loop),
      mysqlPtr_(
          std::shared_ptr<MYSQL>(new MYSQL, [](MYSQL *p) { mysql_close(p); }))
{
    mysql_init(mysqlPtr_.get());
    mysql_options(mysqlPtr_.get(), MYSQL_OPT_NONBLOCK, 0);

    // Get the key and value
    std::regex r(" *= *");
    auto tmpStr = std::regex_replace(connInfo, r, "=");
    std::string host, user, passwd, dbname, port;
    auto keyValues = utils::splitString(tmpStr, " ");
    for (auto const &kvs : keyValues)
    {
        auto kv = utils::splitString(kvs, "=");
        assert(kv.size() == 2);
        auto key = kv[0];
        auto value = kv[1];
        if (value[0] == '\'' && value[value.length() - 1] == '\'')
        {
            value = value.substr(1, value.length() - 2);
        }
        std::transform(key.begin(), key.end(), key.begin(), tolower);
        // LOG_TRACE << key << "=" << value;
        if (key == "host")
        {
            host = value;
        }
        else if (key == "user")
        {
            user = value;
        }
        else if (key == "dbname")
        {
            // LOG_DEBUG << "database:[" << value << "]";
            dbname = value;
        }
        else if (key == "port")
        {
            port = value;
        }
        else if (key == "password")
        {
            passwd = value;
        }
    }
    loop_->queueInLoop([=]() {
        MYSQL *ret;
        status_ = ConnectStatus::Connecting;
        waitStatus_ =
            mysql_real_connect_start(&ret,
                                     mysqlPtr_.get(),
                                     host.empty() ? NULL : host.c_str(),
                                     user.empty() ? NULL : user.c_str(),
                                     passwd.empty() ? NULL : passwd.c_str(),
                                     dbname.empty() ? NULL : dbname.c_str(),
                                     port.empty() ? 3306 : atol(port.c_str()),
                                     NULL,
                                     0);
        // LOG_DEBUG << ret;
        auto fd = mysql_get_socket(mysqlPtr_.get());
        if (fd < 0)
        {
            LOG_FATAL << "Socket fd < 0, Usually this is because the number of "
                         "files opened by the program exceeds the system "
                         "limit. Please use the ulimit command to check.";
            exit(1);
        }
        channelPtr_ =
            std::unique_ptr<trantor::Channel>(new trantor::Channel(loop, fd));
        channelPtr_->setCloseCallback([=]() {
            perror("sock close");
            handleClosed();
        });
        channelPtr_->setEventCallback([=]() { handleEvent(); });
        setChannel();
    });
}

void MysqlConnection::setChannel()
{
    if ((waitStatus_ & MYSQL_WAIT_READ) || (waitStatus_ & MYSQL_WAIT_EXCEPT))
    {
        if (!channelPtr_->isReading())
            channelPtr_->enableReading();
    }
    if (waitStatus_ & MYSQL_WAIT_WRITE)
    {
        if (!channelPtr_->isWriting())
            channelPtr_->enableWriting();
    }
    else
    {
        if (channelPtr_->isWriting())
            channelPtr_->disableWriting();
    }
    if (waitStatus_ & MYSQL_WAIT_TIMEOUT)
    {
        auto timeout = mysql_get_timeout_value(mysqlPtr_.get());
        auto thisPtr = shared_from_this();
        loop_->runAfter(timeout, [thisPtr]() { thisPtr->handleTimeout(); });
    }
}

void MysqlConnection::handleClosed()
{
    loop_->assertInLoopThread();
    if (status_ == ConnectStatus::Bad)
        return;
    status_ = ConnectStatus::Bad;
    channelPtr_->disableAll();
    channelPtr_->remove();
    assert(closeCallback_);
    auto thisPtr = shared_from_this();
    closeCallback_(thisPtr);
}
void MysqlConnection::disconnect()
{
    auto thisPtr = shared_from_this();
    std::promise<int> pro;
    auto f = pro.get_future();
    loop_->runInLoop([thisPtr, &pro]() {
        thisPtr->status_ = ConnectStatus::Bad;
        thisPtr->channelPtr_->disableAll();
        thisPtr->channelPtr_->remove();
        thisPtr->mysqlPtr_.reset();
        pro.set_value(1);
    });
    f.get();
}
void MysqlConnection::handleTimeout()
{
    LOG_TRACE << "channel index:" << channelPtr_->index();
    int status = 0;
    status |= MYSQL_WAIT_TIMEOUT;
    MYSQL *ret;
    if (status_ == ConnectStatus::Connecting)
    {
        waitStatus_ = mysql_real_connect_cont(&ret, mysqlPtr_.get(), status);
        if (waitStatus_ == 0)
        {
            if (!ret)
            {
                handleClosed();
                LOG_ERROR << "Failed to mysql_real_connect()";
                return;
            }
            // I don't think the programe can run to here.
            status_ = ConnectStatus::Ok;
            if (okCallback_)
            {
                auto thisPtr = shared_from_this();
                okCallback_(thisPtr);
            }
        }
        setChannel();
    }
    else if (status_ == ConnectStatus::Ok)
    {
    }
}
void MysqlConnection::handleEvent()
{
    int status = 0;
    auto revents = channelPtr_->revents();
    if (revents & POLLIN)
        status |= MYSQL_WAIT_READ;
    if (revents & POLLOUT)
        status |= MYSQL_WAIT_WRITE;
    if (revents & POLLPRI)
        status |= MYSQL_WAIT_EXCEPT;
    status = (status & waitStatus_);
    if (status == 0 && waitStatus_ != 0)
        return;
    MYSQL *ret;
    if (status_ == ConnectStatus::Connecting)
    {
        waitStatus_ = mysql_real_connect_cont(&ret, mysqlPtr_.get(), status);
        if (waitStatus_ == 0)
        {
            if (!ret)
            {
                handleClosed();
                // perror("");
                LOG_ERROR << "Failed to mysql_real_connect()";
                return;
            }
            status_ = ConnectStatus::Ok;
            if (okCallback_)
            {
                auto thisPtr = shared_from_this();
                okCallback_(thisPtr);
            }
        }
        setChannel();
    }
    else if (status_ == ConnectStatus::Ok)
    {
        switch (execStatus_)
        {
            case ExecStatus::RealQuery:
            {
                int err = 0;
                waitStatus_ =
                    mysql_real_query_cont(&err, mysqlPtr_.get(), status);
                LOG_TRACE << "real_query:" << waitStatus_;
                if (waitStatus_ == 0)
                {
                    if (err)
                    {
                        execStatus_ = ExecStatus::None;
                        LOG_ERROR << "error:" << err << " status:" << status;
                        outputError();
                        return;
                    }
                    execStatus_ = ExecStatus::StoreResult;
                    MYSQL_RES *ret;
                    waitStatus_ =
                        mysql_store_result_start(&ret, mysqlPtr_.get());
                    LOG_TRACE << "store_result_start:" << waitStatus_;
                    if (waitStatus_ == 0)
                    {
                        execStatus_ = ExecStatus::None;
                        if (err)
                        {
                            LOG_ERROR << "error";
                            outputError();
                            return;
                        }
                        getResult(ret);
                    }
                }
                setChannel();
                break;
            }
            case ExecStatus::StoreResult:
            {
                MYSQL_RES *ret;
                waitStatus_ =
                    mysql_store_result_cont(&ret, mysqlPtr_.get(), status);
                LOG_TRACE << "store_result:" << waitStatus_;
                if (waitStatus_ == 0)
                {
                    if (!ret)
                    {
                        execStatus_ = ExecStatus::None;
                        LOG_ERROR << "error";
                        outputError();
                        return;
                    }
                    getResult(ret);
                }
                setChannel();
                break;
            }
            case ExecStatus::None:
            {
                // Connection closed!
                if (waitStatus_ == 0)
                    handleClosed();
                break;
            }
            default:
                return;
        }
    }
}

void MysqlConnection::execSqlInLoop(
    string_view &&sql,
    size_t paraNum,
    std::vector<const char *> &&parameters,
    std::vector<int> &&length,
    std::vector<int> &&format,
    ResultCallback &&rcb,
    std::function<void(const std::exception_ptr &)> &&exceptCallback)
{
    LOG_TRACE << sql;
    assert(paraNum == parameters.size());
    assert(paraNum == length.size());
    assert(paraNum == format.size());
    assert(rcb);
    assert(!isWorking_);
    assert(!sql.empty());

    callback_ = std::move(rcb);
    isWorking_ = true;
    exceptionCallback_ = std::move(exceptCallback);
    sql_.clear();
    if (paraNum > 0)
    {
        std::string::size_type pos = 0;
        std::string::size_type seekPos = std::string::npos;
        for (size_t i = 0; i < paraNum; ++i)
        {
            seekPos = sql.find("?", pos);
            if (seekPos == std::string::npos)
            {
                sql_.append(sql.substr(pos));
                pos = seekPos;
                break;
            }
            else
            {
                sql_.append(sql.substr(pos, seekPos - pos));
                pos = seekPos + 1;
                switch (format[i])
                {
                    case MYSQL_TYPE_TINY:
                        sql_.append(std::to_string(*((char *)parameters[i])));
                        break;
                    case MYSQL_TYPE_SHORT:
                        sql_.append(std::to_string(*((short *)parameters[i])));
                        break;
                    case MYSQL_TYPE_LONG:
                        sql_.append(
                            std::to_string(*((int32_t *)parameters[i])));
                        break;
                    case MYSQL_TYPE_LONGLONG:
                        sql_.append(
                            std::to_string(*((int64_t *)parameters[i])));
                        break;
                    case MYSQL_TYPE_NULL:
                        sql_.append("NULL");
                        break;
                    case MYSQL_TYPE_STRING:
                    {
                        sql_.append("'");
                        std::string to(length[i] * 2, '\0');
                        auto len = mysql_real_escape_string(mysqlPtr_.get(),
                                                            (char *)to.c_str(),
                                                            parameters[i],
                                                            length[i]);
                        to.resize(len);
                        sql_.append(to);
                        sql_.append("'");
                    }
                    break;
                    default:
                        break;
                }
            }
        }
        if (pos < sql.length())
        {
            sql_.append(sql.substr(pos));
        }
    }
    else
    {
        sql_ = sql;
    }
    LOG_TRACE << sql_;
    int err;
    // int mysql_real_query_start(int *ret, MYSQL *mysql, const char *q,
    // unsigned long length)
    waitStatus_ = mysql_real_query_start(&err,
                                         mysqlPtr_.get(),
                                         sql_.c_str(),
                                         sql_.length());
    LOG_TRACE << "real_query:" << waitStatus_;
    execStatus_ = ExecStatus::RealQuery;
    if (waitStatus_ == 0)
    {
        if (err)
        {
            LOG_ERROR << "error";
            outputError();
            return;
        }

        MYSQL_RES *ret;
        waitStatus_ = mysql_store_result_start(&ret, mysqlPtr_.get());
        LOG_TRACE << "store_result:" << waitStatus_;
        execStatus_ = ExecStatus::StoreResult;
        if (waitStatus_ == 0)
        {
            execStatus_ = ExecStatus::None;
            if (!ret)
            {
                LOG_ERROR << "error";
                outputError();
                return;
            }
            getResult(ret);
        }
    }
    setChannel();
    return;
}

void MysqlConnection::outputError()
{
    channelPtr_->disableAll();
    LOG_ERROR << "Error(" << mysql_errno(mysqlPtr_.get()) << ") ["
              << mysql_sqlstate(mysqlPtr_.get()) << "] \""
              << mysql_error(mysqlPtr_.get()) << "\"";
    if (isWorking_)
    {
        try
        {
            // TODO: exception type
            throw SqlError(mysql_error(mysqlPtr_.get()), sql_);
        }
        catch (...)
        {
            exceptionCallback_(std::current_exception());
            exceptionCallback_ = nullptr;
        }

        callback_ = nullptr;
        isWorking_ = false;
        idleCb_();
    }
}

void MysqlConnection::getResult(MYSQL_RES *res)
{
    auto resultPtr = std::shared_ptr<MYSQL_RES>(res, [](MYSQL_RES *r) {
        mysql_free_result(r);
    });
    auto Result = makeResult(resultPtr,
                             mysql_affected_rows(mysqlPtr_.get()),
                             mysql_insert_id(mysqlPtr_.get()));
    if (isWorking_)
    {
        callback_(Result);
        callback_ = nullptr;
        exceptionCallback_ = nullptr;
        isWorking_ = false;
        idleCb_();
    }
}
