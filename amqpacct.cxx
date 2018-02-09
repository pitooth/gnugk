/*
 * amqpacct.cxx
 *
 * accounting module for GNU Gatekeeper that sends it's messages to AMQP queues
 *
 * Copyright (c) 2018, Jan Willamowius
 *
 * This work is published under the GNU Public License version 2 (GPLv2)
 * see file COPYING for details.
 * We also explicitly grant the right to link this code
 * with the OpenH323/H323Plus and OpenSSL library.
 *
 */

#include "config.h"

#ifdef HAS_LIBRABBITMQ

#include "gkacct.h"
#include "Toolkit.h"
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>


class AMQPAcct : public GkAcctLogger
{
public:
	enum Constants
	{
		/// events recognized by this module
		AMQPAcctEvents = AcctStart | AcctStop | AcctUpdate | AcctConnect | AcctAlert | AcctRegister | AcctUnregister | AcctOn | AcctOff | AcctReject
	};

	AMQPAcct(
		/// name from Gatekeeper::Acct section
		const char* moduleName,
		/// config section name to be used with an instance of this module,
		/// pass NULL to use a default section (named "moduleName")
		const char* cfgSecName = NULL
		);

	/// Destroy the accounting logger
	virtual ~AMQPAcct();

	/// overridden from GkAcctLogger
	virtual Status Log(AcctEvent evt, const callptr & call);

	/// overridden from GkAcctLogger
	virtual Status Log(AcctEvent evt, const endptr & ep);

protected:
	virtual Status AMQPLog(const PString & event);

private:
	AMQPAcct();
	/* No copy constructor allowed */
	AMQPAcct(const AMQPAcct &);
	/* No operator= allowed */
	AMQPAcct & operator=(const AMQPAcct &);

private:
    bool m_active;
    PString m_host;
    WORD m_port;
    PString m_user;
    PString m_password;
    PString m_exchange;
    PString m_routingKey;
    PString m_vhost;
    bool m_useSSL;
    PString m_caCert;
    PString m_contentType;
    int m_channelID;
    amqp_socket_t * m_socket;
    amqp_connection_state_t m_conn;

	/// parametrized strings for the events
	PString m_startEvent;
	PString m_stopEvent;
	PString m_updateEvent;
	PString m_connectEvent;
	PString m_alertEvent;
	PString m_registerEvent;
	PString m_unregisterEvent;
	PString m_onEvent;
	PString m_offEvent;
	PString m_rejectEvent;
	/// timestamp formatting string
	PString m_timestampFormat;
};


AMQPAcct::AMQPAcct(const char* moduleName, const char* cfgSecName)
    : GkAcctLogger(moduleName, cfgSecName)
{
	// it is very important to set what type of accounting events
	// are supported for each accounting module, otherwise the Log method
	// will no get called
	SetSupportedEvents(AMQPAcctEvents);

	PConfig* cfg = GetConfig();
	const PString & cfgSec = GetConfigSectionName();

	m_host = cfg->GetString(cfgSec, "Host", "localhost");
	m_port = (WORD)cfg->GetInteger(cfgSec, "Port", 5672);
	m_user = cfg->GetString(cfgSec, "User", "guest");
	m_password = cfg->GetString(cfgSec, "Password", "guest");
	m_exchange = cfg->GetString(cfgSec, "Exchange", "");
	m_routingKey = cfg->GetString(cfgSec, "RoutingKey", "");
	m_vhost = cfg->GetString(cfgSec, "VHost", "/");
	m_useSSL = cfg->GetBoolean(cfgSec, "UseSSL", false);
	m_caCert = cfg->GetString(cfgSec, "CACert", "");
	m_contentType = cfg->GetString(cfgSec, "ContentType", "text/plain");
	m_channelID = 1;

	m_timestampFormat = cfg->GetString(cfgSec, "TimestampFormat", "");
	m_startEvent = cfg->GetString(cfgSec, "StartEvent", "");
	m_stopEvent = cfg->GetString(cfgSec, "StopEvent", "");
	m_updateEvent = cfg->GetString(cfgSec, "UpdateEvent", "");
	m_connectEvent = cfg->GetString(cfgSec, "ConnectEvent", "");
	m_alertEvent = cfg->GetString(cfgSec, "AlertEvent", "");
	m_registerEvent = cfg->GetString(cfgSec, "RegisterEvent", "");
	m_unregisterEvent = cfg->GetString(cfgSec, "UnregisterEvent", "");
	m_onEvent = cfg->GetString(cfgSec, "OnEvent", "");
	m_offEvent = cfg->GetString(cfgSec, "OffEvent", "");
	m_rejectEvent = cfg->GetString(cfgSec, "RejectEvent", "");

	m_active = false;
	int status = 0;
    m_conn = amqp_new_connection();
    if (m_useSSL) {
        m_socket = amqp_ssl_socket_new(m_conn);
        if (!m_socket) {
            PTRACE(1, "AMQPAcct\tError creating SSL socket");
        } else {
            if (!m_caCert.IsEmpty()) {
                status = amqp_ssl_socket_set_cacert(m_socket, (const char *)m_caCert);
                if (status) {
                    PTRACE(1, "AMQPAcct\tError setting CA certificate");
                }
                amqp_ssl_socket_set_verify(m_socket, 0);
                //amqp_ssl_socket_set_verify_peer(m_socket, false);
                //amqp_ssl_socket_set_verify_hostname(m_socket, false);
            }
        }
    } else {
        m_socket = amqp_tcp_socket_new(m_conn);
        if (!m_socket) {
            PTRACE(1, "AMQPAcct\tError creating TCP socket");
        }
    }
    if (m_socket) {
        status = amqp_socket_open(m_socket, (const char*)m_host, m_port);
        if (status) {
            PTRACE(1, "AMQPAcct\tError opening socket to " << m_host << ":" << m_port);
        } else {
            amqp_rpc_reply_t r = amqp_login(m_conn, (const char*)m_vhost, 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, (const char*)m_user, (const char*)m_password);
            if (r.reply_type != AMQP_RESPONSE_NORMAL) {
                PTRACE(1, "AMQPAcct\Error logging in: vhost=" << m_vhost << " user=" << m_user);
            } else {
                amqp_channel_open(m_conn, m_channelID);
                r = amqp_get_rpc_reply(m_conn);
                if (r.reply_type != AMQP_RESPONSE_NORMAL) {
                    PTRACE(1, "AMQPAcct\tError opening channel");
                } else {
                    m_active = true;
                }
            }
        }
    }
}

AMQPAcct::~AMQPAcct()
{
    m_active = false;
    amqp_channel_close(m_conn, m_channelID, AMQP_REPLY_SUCCESS);
    amqp_connection_close(m_conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(m_conn);
}

GkAcctLogger::Status AMQPAcct::Log(GkAcctLogger::AcctEvent evt, const callptr & call)
{
	// a workaround to prevent processing end on "sufficient" module
	// if it is not interested in this event type
	if ((evt & GetEnabledEvents() & GetSupportedEvents()) == 0)
		return Next;

    if (!m_active) {
		PTRACE(1, "AMQPAcct\t" << GetName() << " not ready");
		return Fail;
    }

	if (!call && evt != AcctOn && evt != AcctOff) {
		PTRACE(1, "AMQPAcct\t" << GetName() << " - missing call info for event " << evt);
		return Fail;
	}

	PString event;
	if (evt == AcctStart) {
		event = m_startEvent;
	} else if (evt == AcctConnect) {
		event = m_connectEvent;
	} else if (evt == AcctUpdate) {
		event = m_updateEvent;
	} else if (evt == AcctStop) {
		event = m_stopEvent;
	} else if (evt == AcctAlert) {
		event = m_alertEvent;
	} else if (evt == AcctOn) {
		event = m_onEvent;
	} else if (evt == AcctOff) {
		event = m_offEvent;
	} else if (evt == AcctReject) {
		event = m_rejectEvent;
	}

	if (event.IsEmpty()) {
		PTRACE(1, "AMQPAcct\t" << GetName() << "Error: No message configured for event " << evt);
		return Fail;
	}

    std::map<PString, PString> params;
    SetupAcctParams(params, call, m_timestampFormat);
    event = ReplaceAcctParams(event, params);
    event = Toolkit::Instance()->ReplaceGlobalParams(event);

	return AMQPLog(event);
}

GkAcctLogger::Status AMQPAcct::Log(GkAcctLogger::AcctEvent evt, const endptr & ep)
{
	// a workaround to prevent processing end on "sufficient" module
	// if it is not interested in this event type
	if ((evt & GetEnabledEvents() & GetSupportedEvents()) == 0)
		return Next;

    if (!m_active) {
		PTRACE(1, "AMQPAcct\t" << GetName() << " not ready");
		return Fail;
    }

	if (!ep) {
		PTRACE(1, "AMQPAcct\t" << GetName() << " - missing endpoint info for event " << evt);
		return Fail;
	}

	PString event;
	if (evt == AcctRegister) {
		event = m_registerEvent;
	} else if (evt == AcctUnregister) {
		event = m_unregisterEvent;
	}

	if (event.IsEmpty()) {
		PTRACE(1, "AMQPAcct\t" << GetName() << "Error: No message configured for event " << evt);
		return Fail;
	}

    std::map<PString, PString> params;
    SetupAcctEndpointParams(params, ep, m_timestampFormat);
    event = ReplaceAcctParams(event, params);
    event = Toolkit::Instance()->ReplaceGlobalParams(event);

	return AMQPLog(event);
}

GkAcctLogger::Status AMQPAcct::AMQPLog(const PString & event)
{
    PTRACE(5, "AMQPAcct\tLogging message=" << event);
    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes((const char *)m_contentType);
    props.delivery_mode = 2; /* persistent delivery mode */
    int status = amqp_basic_publish(m_conn, m_channelID, amqp_cstring_bytes((const char *)m_exchange),
                                    amqp_cstring_bytes((const char *)m_routingKey), 0, 0,
                                    &props, amqp_cstring_bytes((const char *)event));
    if (status) {
        PTRACE(1, "AMQPAcct\tError publishing event");
        return Fail;
    }

    return Ok;
}


namespace {
	// append accounting logger to the global list of loggers
	GkAcctLoggerCreator<AMQPAcct> AMQPAcctCreator("AMQPAcct");
}

#endif // HAS_LIBRABBITMQ