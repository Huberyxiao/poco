//
// HTTPClientSession.cpp
//
// Library: Net
// Package: HTTPClient
// Module:  HTTPClientSession
//
// Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/Net/HTTPHeaderStream.h"
#include "Poco/Net/HTTPStream.h"
#include "Poco/Net/HTTPFixedLengthStream.h"
#include "Poco/Net/HTTPChunkedStream.h"
#include "Poco/Net/HTTPBasicCredentials.h"
#include "Poco/Net/NetException.h"
#include "Poco/NumberFormatter.h"
#include "Poco/CountingStream.h"
#include "Poco/RegularExpression.h"
#include <sstream>

#include "Poco/Logger.h"
#include "Poco/Message.h"
#include "Poco/ConsoleChannel.h"
#include "Poco/PatternFormatter.h"
#include "Poco/FormattingChannel.h"
#include "Poco/DateTimeFormatter.h"

using Poco::NumberFormatter;
using Poco::IllegalStateException;


namespace Poco {
namespace Net {

class CustomPatternFormatter : public Poco::PatternFormatter
{
public:
    CustomPatternFormatter(const std::string& format) : Poco::PatternFormatter(format) {}

protected:
    virtual void format(const Poco::Message& msg, std::string& text) override
    {
        Poco::PatternFormatter::format(msg, text);

        // 使用 Poco::LocalDateTime 和 Poco::DateTimeFormatter::append() 方法设置本地时区
        Poco::LocalDateTime now;
        std::string pattern = "%Y-%m-%d %H:%M:%S.%i";
        std::string localTime;
        Poco::DateTimeFormatter::append(localTime, now, pattern);

        // 更新日志消息以使用本地时区
        text.replace(0, localTime.length(), localTime);
    }
};

HTTPClientSession::ProxyConfig HTTPClientSession::_globalProxyConfig;


HTTPClientSession::HTTPClientSession():
	_port(HTTPSession::HTTP_PORT),
	_proxyConfig(_globalProxyConfig),
	_keepAliveTimeout(DEFAULT_KEEP_ALIVE_TIMEOUT, 0),
	_reconnect(false),
	_mustReconnect(false),
	_expectResponseBody(false),
	_responseReceived(false)
{
}


HTTPClientSession::HTTPClientSession(const StreamSocket& socket):
	HTTPSession(socket),
	_port(HTTPSession::HTTP_PORT),
	_proxyConfig(_globalProxyConfig),
	_keepAliveTimeout(DEFAULT_KEEP_ALIVE_TIMEOUT, 0),
	_reconnect(false),
	_mustReconnect(false),
	_expectResponseBody(false),
	_responseReceived(false)
{
}


HTTPClientSession::HTTPClientSession(const SocketAddress& address):
	_host(address.host().toString()),
	_port(address.port()),
	_proxyConfig(_globalProxyConfig),
	_keepAliveTimeout(DEFAULT_KEEP_ALIVE_TIMEOUT, 0),
	_reconnect(false),
	_mustReconnect(false),
	_expectResponseBody(false),
	_responseReceived(false)
{
}


HTTPClientSession::HTTPClientSession(const std::string& host, Poco::UInt16 port):
	_host(host),
	_port(port),
	_proxyConfig(_globalProxyConfig),
	_keepAliveTimeout(DEFAULT_KEEP_ALIVE_TIMEOUT, 0),
	_reconnect(false),
	_mustReconnect(false),
	_expectResponseBody(false),
	_responseReceived(false)
{
}


HTTPClientSession::HTTPClientSession(const std::string& host, Poco::UInt16 port, const ProxyConfig& proxyConfig):
	_host(host),
	_port(port),
	_proxyConfig(proxyConfig),
	_keepAliveTimeout(DEFAULT_KEEP_ALIVE_TIMEOUT, 0),
	_reconnect(false),
	_mustReconnect(false),
	_expectResponseBody(false),
	_responseReceived(false)
{
}


HTTPClientSession::~HTTPClientSession()
{
}


void HTTPClientSession::setHost(const std::string& host)
{
	if (!connected())
		_host = host;
	else
		throw IllegalStateException("Cannot set the host for an already connected session");
}


void HTTPClientSession::setPort(Poco::UInt16 port)
{
	if (!connected())
		_port = port;
	else
		throw IllegalStateException("Cannot set the port number for an already connected session");
}


void HTTPClientSession::setProxy(const std::string& host, Poco::UInt16 port)
{
	if (!connected())
	{
		_proxyConfig.host = host;
		_proxyConfig.port = port;
	}
	else throw IllegalStateException("Cannot set the proxy host and port for an already connected session");
}


void HTTPClientSession::setProxyHost(const std::string& host)
{
	if (!connected())
		_proxyConfig.host = host;
	else
		throw IllegalStateException("Cannot set the proxy host for an already connected session");
}


void HTTPClientSession::setProxyPort(Poco::UInt16 port)
{
	if (!connected())
		_proxyConfig.port = port;
	else
		throw IllegalStateException("Cannot set the proxy port number for an already connected session");
}


void HTTPClientSession::setProxyCredentials(const std::string& username, const std::string& password)
{
	_proxyConfig.username = username;
	_proxyConfig.password = password;
}


void HTTPClientSession::setProxyUsername(const std::string& username)
{
	_proxyConfig.username = username;
}


void HTTPClientSession::setProxyPassword(const std::string& password)
{
	_proxyConfig.password = password;
}


void HTTPClientSession::setProxyConfig(const ProxyConfig& config)
{
	_proxyConfig = config;
}


void HTTPClientSession::setGlobalProxyConfig(const ProxyConfig& config)
{
	_globalProxyConfig = config;
}


void HTTPClientSession::setKeepAliveTimeout(const Poco::Timespan& timeout)
{
	_keepAliveTimeout = timeout;
}


std::ostream& HTTPClientSession::sendRequest(HTTPRequest& request)
{
	Poco::Logger& _logger = Poco::Logger::get("PocoHTTPClientSession");
	Poco::AutoPtr<CustomPatternFormatter> pFormatter(new CustomPatternFormatter("%Y-%m-%d %H:%M:%S.%i [%p] %t"));
    Poco::AutoPtr<Poco::FormattingChannel> pFormattingChannel(new Poco::FormattingChannel(pFormatter));
    Poco::AutoPtr<Poco::ConsoleChannel> pConsoleChannel(new Poco::ConsoleChannel);
    pFormattingChannel->setChannel(pConsoleChannel);

    _logger.setChannel(pFormattingChannel);
    _logger.setLevel(Poco::Message::PRIO_DEBUG);


	_logger.debug("sendRequest Start! : " + request.getURI());
	_pRequestStream = 0;
	_pResponseStream = 0;
	clearException();
	_responseReceived = false;

	_logger.debug("sendRequest: Initial Close Start : " + request.getURI());
	bool keepAlive = getKeepAlive();
	if (((connected() && !keepAlive) || mustReconnect()) && !_host.empty())
	{
		close();
		_mustReconnect = false;
	}
	_logger.debug("sendRequest: Initial Close Done : " + request.getURI());
	try
	{
		if (!connected())
			_logger.debug("sendRequest: Reconnect Start : " + request.getURI());
			reconnect();
			_logger.debug("sendRequest: Reconnect Done : " + request.getURI());
		if (!keepAlive)
			request.setKeepAlive(false);
		if (!request.has(HTTPRequest::HOST) && !_host.empty())
			request.setHost(_host, _port);
		if (!_proxyConfig.host.empty() && !bypassProxy())
		{
			_logger.debug("sendRequest: proxyAuthenticate Start : " + request.getURI());
			request.setURI(proxyRequestPrefix() + request.getURI());
			proxyAuthenticate(request);
			_logger.debug("sendRequest: proxyAuthenticate Done : " + request.getURI());
		}
		_reconnect = keepAlive;
		_expectResponseBody = request.getMethod() != HTTPRequest::HTTP_HEAD;
		const std::string& method = request.getMethod();
		if (request.getChunkedTransferEncoding())
		{
			HTTPHeaderOutputStream hos(*this);
			request.write(hos);
			_pRequestStream = new HTTPChunkedOutputStream(*this);
		}
		else if (request.hasContentLength())
		{
			_logger.debug("sendRequest: write Start : " + request.getURI());
			Poco::CountingOutputStream cs;
			request.write(cs);
#if POCO_HAVE_INT64
			_pRequestStream = new HTTPFixedLengthOutputStream(*this, request.getContentLength64() + cs.chars());
#else
			_pRequestStream = new HTTPFixedLengthOutputStream(*this, request.getContentLength() + cs.chars());
#endif
			request.write(*_pRequestStream);
			_logger.debug("sendRequest: write Done : " + request.getURI());
		}
		else if ((method != HTTPRequest::HTTP_PUT && method != HTTPRequest::HTTP_POST && method != HTTPRequest::HTTP_PATCH) || request.has(HTTPRequest::UPGRADE))
		{
			Poco::CountingOutputStream cs;
			request.write(cs);
			_pRequestStream = new HTTPFixedLengthOutputStream(*this, cs.chars());
			request.write(*_pRequestStream);
		}
		else
		{
			_pRequestStream = new HTTPOutputStream(*this);
			request.write(*_pRequestStream);
		}
		_lastRequest.update();
		_logger.debug("sendRequest Done! : " + request.getURI());
		return *_pRequestStream;
	}
	catch (Exception&)
	{
		close();
		throw;
	}
}


void HTTPClientSession::flushRequest()
{
	_pRequestStream = 0;
	if (networkException()) networkException()->rethrow();
}


std::istream& HTTPClientSession::receiveResponse(HTTPResponse& response)
{
	flushRequest();
	if (!_responseReceived)
	{
		do
		{
			response.clear();
			HTTPHeaderInputStream his(*this);
			try
			{
				response.read(his);
			}
			catch (Exception&)
			{
				close();
				if (networkException())
					networkException()->rethrow();
				else
					throw;
				throw;
			}
		}
		while (response.getStatus() == HTTPResponse::HTTP_CONTINUE);
	}

	_mustReconnect = getKeepAlive() && !response.getKeepAlive();

	if (!_expectResponseBody || response.getStatus() < 200 || response.getStatus() == HTTPResponse::HTTP_NO_CONTENT || response.getStatus() == HTTPResponse::HTTP_NOT_MODIFIED)
		_pResponseStream = new HTTPFixedLengthInputStream(*this, 0);
	else if (response.getChunkedTransferEncoding())
		_pResponseStream = new HTTPChunkedInputStream(*this);
	else if (response.hasContentLength())
#if defined(POCO_HAVE_INT64)
		_pResponseStream = new HTTPFixedLengthInputStream(*this, response.getContentLength64());
#else
		_pResponseStream = new HTTPFixedLengthInputStream(*this, response.getContentLength());
#endif
	else
		_pResponseStream = new HTTPInputStream(*this);

	return *_pResponseStream;
}


bool HTTPClientSession::peekResponse(HTTPResponse& response)
{
	poco_assert (!_responseReceived);

	_pRequestStream->flush();

	if (networkException()) networkException()->rethrow();

	response.clear();
	HTTPHeaderInputStream his(*this);
	try
	{
		response.read(his);
	}
	catch (Exception&)
	{
		close();
		if (networkException())
			networkException()->rethrow();
		else
			throw;
		throw;
	}
	_responseReceived = response.getStatus() != HTTPResponse::HTTP_CONTINUE;
	return !_responseReceived;
}


void HTTPClientSession::reset()
{
	close();
}


bool HTTPClientSession::secure() const
{
	return false;
}


int HTTPClientSession::write(const char* buffer, std::streamsize length)
{
	try
	{
		int rc = HTTPSession::write(buffer, length);
		_reconnect = false;
		return rc;
	}
	catch (IOException&)
	{
		if (_reconnect)
		{
			close();
			reconnect();
			int rc = HTTPSession::write(buffer, length);
			clearException();
			_reconnect = false;
			return rc;
		}
		else throw;
	}
}


void HTTPClientSession::reconnect()
{
	if (_proxyConfig.host.empty() || bypassProxy())
	{
		SocketAddress addr(_host, _port);
		connect(addr);
	}
	else
	{
		SocketAddress addr(_proxyConfig.host, _proxyConfig.port);
		connect(addr);
	}
}


std::string HTTPClientSession::proxyRequestPrefix() const
{
	std::string result("http://");
	result.append(_host);
	result.append(":");
	NumberFormatter::append(result, _port);
	return result;
}


bool HTTPClientSession::mustReconnect() const
{
	if (!_mustReconnect)
	{
		Poco::Timestamp now;
		return _keepAliveTimeout <= now - _lastRequest;
	}
	else return true;
}


void HTTPClientSession::proxyAuthenticate(HTTPRequest& request)
{
	proxyAuthenticateImpl(request);
}


void HTTPClientSession::proxyAuthenticateImpl(HTTPRequest& request)
{
	if (!_proxyConfig.username.empty())
	{
		HTTPBasicCredentials creds(_proxyConfig.username, _proxyConfig.password);
		creds.proxyAuthenticate(request);
	}
}


StreamSocket HTTPClientSession::proxyConnect()
{
	ProxyConfig emptyProxyConfig;
	HTTPClientSession proxySession(getProxyHost(), getProxyPort(), emptyProxyConfig);
	proxySession.setTimeout(getTimeout());
	std::string targetAddress(_host);
	targetAddress.append(":");
	NumberFormatter::append(targetAddress, _port);
	HTTPRequest proxyRequest(HTTPRequest::HTTP_CONNECT, targetAddress, HTTPMessage::HTTP_1_1);
	HTTPResponse proxyResponse;
	proxyRequest.set("Proxy-Connection", "keep-alive");
	proxyRequest.set("Host", getHost());
	proxyAuthenticateImpl(proxyRequest);
	proxySession.setKeepAlive(true);
	proxySession.sendRequest(proxyRequest);
	proxySession.receiveResponse(proxyResponse);
	if (proxyResponse.getStatus() != HTTPResponse::HTTP_OK)
		throw HTTPException("Cannot establish proxy connection", proxyResponse.getReason());
	return proxySession.detachSocket();
}


void HTTPClientSession::proxyTunnel()
{
	StreamSocket ss = proxyConnect();
	attachSocket(ss);
}


bool HTTPClientSession::bypassProxy() const
{
	if (!_proxyConfig.nonProxyHosts.empty())
	{
		return RegularExpression::match(_host, _proxyConfig.nonProxyHosts, RegularExpression::RE_CASELESS | RegularExpression::RE_ANCHORED);
	}
	else return false;
}


} } // namespace Poco::Net
