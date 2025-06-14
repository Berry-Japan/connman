/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2013  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>

#include <gweb/gweb.h>

#include "connman.h"

struct connman_wispr_message {
	bool has_error;
	const char *current_element;
	int message_type;
	int response_code;
	char *login_url;
	char *abort_login_url;
	char *logoff_url;
	char *access_procedure;
	char *access_location;
	char *location_name;
};

enum connman_wispr_result {
	CONNMAN_WISPR_RESULT_UNKNOWN = 0,
	CONNMAN_WISPR_RESULT_LOGIN   = 1,
	CONNMAN_WISPR_RESULT_ONLINE  = 2,
	CONNMAN_WISPR_RESULT_FAILED  = 3,
};

/**
 *  State for host routes used for a WISPr request.
 */
struct wispr_route {
	/**
	 *	A pointer to a mutable, dynamically-allocated null-terminated
	 *	C string containing the text-formatted address of the WISPr
	 *	host.
	 */
	char *address;

	/**
	 *	The network interface index associated with the underlying
	 *	network interface over which the WISPr request will sent and
	 *	the reply received.
	 */
	int if_index;

	/**
	 *	The route metric/priority used for the host route created for
	 *	the WISPr host.
	 */
	uint32_t metric;
};

/**
 *  Gateway configuration function pointers for IP configuration
 *  type-specific route set/clear/add/delete operations.
 */
struct wispr_portal_context_route_ops {
	int (*add_host_route_with_metric)(int index,
		const char *host,
		const char *gateway,
		uint32_t metric);
	int (*del_host_route_with_metric)(int index,
		const char *host,
		const char *gateway,
		uint32_t metric);
};

struct connman_wispr_portal_context {
	int refcount;
	struct connman_service *service;
	enum connman_ipconfig_type type;

	/**
	 *	A pointer to immutable function pointers for route
	 *	set/clear/add/delete operations.
	 */
	const struct wispr_portal_context_route_ops *ops;

	__connman_wispr_cb_t cb;
	struct connman_wispr_portal *wispr_portal;

	/* Portal/WISPr common */
	GWeb *web;
	unsigned int proxy_token;
	guint request_id;

	const char *status_url;

	char *redirect_url;

	/* WISPr specific */
	GWebParser *wispr_parser;
	struct connman_wispr_message wispr_msg;

	char *wispr_username;
	char *wispr_password;
	char *wispr_formdata;

	enum connman_wispr_result wispr_result;

	GSList *route_list;

	guint proxy_timeout;
};

struct connman_wispr_portal {
	struct connman_wispr_portal_context *ipv4_context;
	struct connman_wispr_portal_context *ipv6_context;
};

static bool wispr_portal_web_result(const GError *error, GWebResult *result, gpointer user_data);

/**
 *  A dictionary / hash table of network interface indices to
 *  active / outstanding WISPr / portal request contexts.
 *
 */
static GHashTable *wispr_portal_hash = NULL;

static char *online_check_ipv4_url = NULL;
static char *online_check_ipv6_url = NULL;

static const struct wispr_portal_context_route_ops
	ipv4_wispr_portal_context_route_ops = {
	.add_host_route_with_metric    =
		connman_inet_add_host_route_with_metric,
	.del_host_route_with_metric    =
		connman_inet_del_host_route_with_metric
};

static const struct wispr_portal_context_route_ops
	ipv6_wispr_portal_context_route_ops = {
	.add_host_route_with_metric    =
		connman_inet_add_ipv6_host_route_with_metric,
	.del_host_route_with_metric    =
		connman_inet_del_ipv6_host_route_with_metric
};

#define wispr_portal_context_ref(wp_context) \
	wispr_portal_context_ref_debug(wp_context, __FILE__, __LINE__, __func__)
#define wispr_portal_context_unref(wp_context) \
	wispr_portal_context_unref_debug(wp_context, __FILE__, __LINE__, __func__)

static void connman_wispr_message_init(struct connman_wispr_message *msg)
{
	msg->has_error = false;
	msg->current_element = NULL;

	msg->message_type = -1;
	msg->response_code = -1;

	g_free(msg->login_url);
	msg->login_url = NULL;

	g_free(msg->abort_login_url);
	msg->abort_login_url = NULL;

	g_free(msg->logoff_url);
	msg->logoff_url = NULL;

	g_free(msg->access_procedure);
	msg->access_procedure = NULL;

	g_free(msg->access_location);
	msg->access_location = NULL;

	g_free(msg->location_name);
	msg->location_name = NULL;
}

/**
 *  @brief
 *    Deallocate resources associated with the specified WISPr host
 *    route.
 *
 *  This attempts to deallocate resources, including host routes,
 *  associated with the specified WISPr host route belonging to the
 *  specified WISPr portal context.
 *
 *  @param[in]      wp_context  A pointer to the immutable WISPr
 *                              portal context associated with @a
 *                              route.
 *  @param[in,out]  route       A pointer to the mutable WISPr host
 *                              route structure for which to delete an
 *                              associated host route and to
 *                              deallocate any dynamically-allocated
 *                              resources associated with it.
 *
 *  @sa wispr_route_request
 *
 */
static void free_wispr_route(
		const struct connman_wispr_portal_context *wp_context,
		struct wispr_route *route)
{
	g_autofree char *interface = NULL;
	const char *gateway;

	gateway = __connman_ipconfig_get_gateway_from_index(route->if_index,
		wp_context->type);

	/*
	 * If gateway was null, as with wispr_route_request, there was no
	 * host route created. Consequently, there is no host route to
	 * delete. Simply free the address.
	 */
	if (!gateway)
		goto free;

	interface = connman_inet_ifname(route->if_index);

	DBG("delete route to %s via %s dev %d (%s) metric %u type %d (%s) "
		"ops %p",
		route->address,
		gateway,
		route->if_index, interface,
		route->metric,
		wp_context->type,
		__connman_ipconfig_type2string(wp_context->type),
		wp_context->ops);

	wp_context->ops->del_host_route_with_metric(
				route->if_index,
				route->address,
				gateway,
				route->metric);

free:
	g_free(route->address);
	route->address = NULL;
}

/**
 *  @brief
 *    Deallocate host route resources associated with the specified
 *    WISPr portal context.
 *
 *  This attempts to deallocate host route resources associated with
 *  the specified WISPr specified WISPr portal context.
 *
 *  @param[in]      wp_context  A pointer to the mutable WISPr
 *                              portal context for which to deallocate
 *                              resources associated with host routes.
 *
 */
static void free_wispr_routes(struct connman_wispr_portal_context *wp_context)
{
	while (wp_context->route_list) {
		struct wispr_route *route = wp_context->route_list->data;

		free_wispr_route(wp_context, route);

		g_free(route);
		wp_context->route_list->data = NULL;

		wp_context->route_list =
			g_slist_delete_link(wp_context->route_list,
					wp_context->route_list);
	}
}

/**
 *  @brief
 *    Cancel a WISPr/portal request.
 *
 *  This attempts to cancel any outstanding request associated with
 *  the specified WISPr/portal context. This deallocates any resources
 *  associated with the context, except for the context itself which
 *  requires invoking #free_connman_wispr_portal_context.
 *
 *  @param[in,out]  wp_context  A pointer to the mutable WISPr/portal
 *                              context for which to cancel an outstanding
 *                              request, whether a reachability check
 *                              or otherwise.
 *
 *  @sa create_connman_wispr_portal_context
 *  @sa free_connman_wispr_portal_context
 *
 */
static void cancel_connman_wispr_portal_context(
		struct connman_wispr_portal_context *wp_context)
{
	if (!wp_context)
		return;

	DBG("wispr/portal context %p service %p (%s) type %d (%s)",
		wp_context,
		wp_context->service,
		connman_service_get_identifier(wp_context->service),
		wp_context->type,
		__connman_ipconfig_type2string(wp_context->type));

	if (wp_context->proxy_token > 0) {
		connman_proxy_lookup_cancel(wp_context->proxy_token);
		wp_context->proxy_token = 0;
	}

	if (wp_context->request_id > 0) {
		g_web_cancel_request(wp_context->web, wp_context->request_id);
		wp_context->request_id = 0;
	}

	if (wp_context->proxy_timeout > 0) {
		g_source_remove(wp_context->proxy_timeout);
		wp_context->proxy_timeout = 0;
	}

	if (wp_context->web) {
		g_web_unref(wp_context->web);
		wp_context->web = NULL;
	}

	g_free(wp_context->redirect_url);
	wp_context->redirect_url = NULL;

	if (wp_context->wispr_parser) {
		g_web_parser_unref(wp_context->wispr_parser);
		wp_context->wispr_parser = NULL;
	}

	connman_wispr_message_init(&wp_context->wispr_msg);

	g_free(wp_context->wispr_username);
	wp_context->wispr_username = NULL;

	g_free(wp_context->wispr_password);
	wp_context->wispr_password = NULL;

	g_free(wp_context->wispr_formdata);
	wp_context->wispr_formdata = NULL;

	free_wispr_routes(wp_context);
}

static void free_connman_wispr_portal_context(
		struct connman_wispr_portal_context *wp_context)
{
	DBG("wispr/portal context %p service %p (%s) type %d (%s)",
		wp_context,
		wp_context->service,
		connman_service_get_identifier(wp_context->service),
		wp_context->type,
		__connman_ipconfig_type2string(wp_context->type));

	if (wp_context->wispr_portal) {
		if (wp_context->wispr_portal->ipv4_context == wp_context)
			wp_context->wispr_portal->ipv4_context = NULL;

		if (wp_context->wispr_portal->ipv6_context == wp_context)
			wp_context->wispr_portal->ipv6_context = NULL;
	}

	cancel_connman_wispr_portal_context(wp_context);

	g_free(wp_context);
}

static struct connman_wispr_portal_context *
wispr_portal_context_ref_debug(struct connman_wispr_portal_context *wp_context,
			const char *file, int line, const char *caller)
{
	DBG("%p ref %d by %s:%d:%s()", wp_context,
		wp_context->refcount + 1, file, line, caller);

	__sync_fetch_and_add(&wp_context->refcount, 1);

	return wp_context;
}

static void wispr_portal_context_unref_debug(
		struct connman_wispr_portal_context *wp_context,
		const char *file, int line, const char *caller)
{
	if (!wp_context)
		return;

	DBG("%p ref %d by %s:%d:%s()", wp_context,
		wp_context->refcount - 1, file, line, caller);

	if (__sync_fetch_and_sub(&wp_context->refcount, 1) != 1)
		return;

	free_connman_wispr_portal_context(wp_context);
}

static struct connman_wispr_portal_context *create_wispr_portal_context(void)
{
	return wispr_portal_context_ref(
		g_new0(struct connman_wispr_portal_context, 1));
}

static void free_connman_wispr_portal(gpointer data)
{
	struct connman_wispr_portal *wispr_portal = data;

	DBG("");

	if (!wispr_portal)
		return;

	wispr_portal_context_unref(wispr_portal->ipv4_context);
	wispr_portal_context_unref(wispr_portal->ipv6_context);

	g_free(wispr_portal);
}

static const char *message_type_to_string(int message_type)
{
	switch (message_type) {
	case 100:
		return "Initial redirect message";
	case 110:
		return "Proxy notification";
	case 120:
		return "Authentication notification";
	case 130:
		return "Logoff notification";
	case 140:
		return "Response to Authentication Poll";
	case 150:
		return "Response to Abort Login";
	}

	return NULL;
}

static const char *response_code_to_string(int response_code)
{
	switch (response_code) {
	case 0:
		return "No error";
	case 50:
		return "Login succeeded";
	case 100:
		return "Login failed";
	case 102:
		return "RADIUS server error/timeout";
	case 105:
		return "RADIUS server not enabled";
	case 150:
		return "Logoff succeeded";
	case 151:
		return "Login aborted";
	case 200:
		return "Proxy detection/repeat operation";
	case 201:
		return "Authentication pending";
	case 255:
		return "Access gateway internal error";
	}

	return NULL;
}

static struct {
	const char *str;
	enum {
		WISPR_ELEMENT_NONE              = 0,
		WISPR_ELEMENT_ACCESS_PROCEDURE  = 1,
		WISPR_ELEMENT_ACCESS_LOCATION   = 2,
		WISPR_ELEMENT_LOCATION_NAME     = 3,
		WISPR_ELEMENT_LOGIN_URL         = 4,
		WISPR_ELEMENT_ABORT_LOGIN_URL   = 5,
		WISPR_ELEMENT_MESSAGE_TYPE      = 6,
		WISPR_ELEMENT_RESPONSE_CODE     = 7,
		WISPR_ELEMENT_NEXT_URL          = 8,
		WISPR_ELEMENT_DELAY             = 9,
		WISPR_ELEMENT_REPLY_MESSAGE     = 10,
		WISPR_ELEMENT_LOGIN_RESULTS_URL = 11,
		WISPR_ELEMENT_LOGOFF_URL        = 12,
	} element;
} wispr_element_map[] = {
	{ "AccessProcedure",	WISPR_ELEMENT_ACCESS_PROCEDURE	},
	{ "AccessLocation",	WISPR_ELEMENT_ACCESS_LOCATION	},
	{ "LocationName",	WISPR_ELEMENT_LOCATION_NAME	},
	{ "LoginURL",		WISPR_ELEMENT_LOGIN_URL		},
	{ "AbortLoginURL",	WISPR_ELEMENT_ABORT_LOGIN_URL	},
	{ "MessageType",	WISPR_ELEMENT_MESSAGE_TYPE	},
	{ "ResponseCode",	WISPR_ELEMENT_RESPONSE_CODE	},
	{ "NextURL",		WISPR_ELEMENT_NEXT_URL		},
	{ "Delay",		WISPR_ELEMENT_DELAY		},
	{ "ReplyMessage",	WISPR_ELEMENT_REPLY_MESSAGE	},
	{ "LoginResultsURL",	WISPR_ELEMENT_LOGIN_RESULTS_URL	},
	{ "LogoffURL",		WISPR_ELEMENT_LOGOFF_URL	},
	{ NULL,			WISPR_ELEMENT_NONE		},
};

static void xml_wispr_start_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					const gchar **attribute_names,
					const gchar **attribute_values,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;

	msg->current_element = element_name;
}

static void xml_wispr_end_element_handler(GMarkupParseContext *context,
					const gchar *element_name,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;

	msg->current_element = NULL;
}

static void xml_wispr_text_handler(GMarkupParseContext *context,
					const gchar *text, gsize text_len,
					gpointer user_data, GError **error)
{
	struct connman_wispr_message *msg = user_data;
	int i;

	if (!msg->current_element)
		return;

	for (i = 0; wispr_element_map[i].str; i++) {
		if (!g_str_equal(wispr_element_map[i].str, msg->current_element))
			continue;

		switch (wispr_element_map[i].element) {
		case WISPR_ELEMENT_NONE:
		case WISPR_ELEMENT_ACCESS_PROCEDURE:
			g_free(msg->access_procedure);
			msg->access_procedure = g_strdup(text);
			break;
		case WISPR_ELEMENT_ACCESS_LOCATION:
			g_free(msg->access_location);
			msg->access_location = g_strdup(text);
			break;
		case WISPR_ELEMENT_LOCATION_NAME:
			g_free(msg->location_name);
			msg->location_name = g_strdup(text);
			break;
		case WISPR_ELEMENT_LOGIN_URL:
			g_free(msg->login_url);
			msg->login_url = g_strdup(text);
			break;
		case WISPR_ELEMENT_ABORT_LOGIN_URL:
			g_free(msg->abort_login_url);
			msg->abort_login_url = g_strdup(text);
			break;
		case WISPR_ELEMENT_MESSAGE_TYPE:
			msg->message_type = atoi(text);
			break;
		case WISPR_ELEMENT_RESPONSE_CODE:
			msg->response_code = atoi(text);
			break;
		case WISPR_ELEMENT_NEXT_URL:
		case WISPR_ELEMENT_DELAY:
		case WISPR_ELEMENT_REPLY_MESSAGE:
		case WISPR_ELEMENT_LOGIN_RESULTS_URL:
			break;
		case WISPR_ELEMENT_LOGOFF_URL:
			g_free(msg->logoff_url);
			msg->logoff_url = g_strdup(text);
			break;
		}
	}
}

static void xml_wispr_error_handler(GMarkupParseContext *context,
					GError *error, gpointer user_data)
{
	struct connman_wispr_message *msg = user_data;

	msg->has_error = true;
}

static const GMarkupParser xml_wispr_parser_handlers = {
	xml_wispr_start_element_handler,
	xml_wispr_end_element_handler,
	xml_wispr_text_handler,
	NULL,
	xml_wispr_error_handler,
};

static void xml_wispr_parser_callback(const char *str, gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	GMarkupParseContext *parser_context = NULL;
	bool result;

	DBG("");

	parser_context = g_markup_parse_context_new(&xml_wispr_parser_handlers,
					G_MARKUP_TREAT_CDATA_AS_TEXT,
					&(wp_context->wispr_msg), NULL);

	result = g_markup_parse_context_parse(parser_context,
					str, strlen(str), NULL);
	if (result)
		g_markup_parse_context_end_parse(parser_context, NULL);

	g_markup_parse_context_free(parser_context);
}

static void web_debug(const char *str, void *data)
{
	connman_info("%s: %s\n", (const char *) data, str);
}

static void wispr_portal_error(struct connman_wispr_portal_context *wp_context)
{
	DBG("Failed to proceed wispr/portal web request");

	wp_context->wispr_result = CONNMAN_WISPR_RESULT_FAILED;
}

/**
 *  @brief
 *    Handle an unsuccessful "online" HTTP-based Internet reachability
 *    check.
 *
 *  This handles an unsuccessful (that is, either completed with a
 *  non-successful operating system error or with a non-HTTP 200 "OK"
 *  status code) "online" HTTP-based Internet reachability check
 *  previously-initiated with #__connman_wispr_start.
 *
 *  @param[in,out]  wp_context  A pointer to the mutable WISPr portal
 *                              detection context associated with the
 *                              unsuccessful "online" HTTP-based
 *                              Internet reachability check this is
 *                              handling.
 *  @param[in]      err         The negated POSIX domain error
 *                              associated with the unsuccessful
 *                              "online" HTTP-based Internet
 *                              reachability check.
 *  @param[in]      message     A pointer to an immutable null-
 *                              terminated C string describing the
 *                              reason for the online check failure.
 *
 *  @sa portal_manage_success_status
 *  @sa wispr_portal_web_result_failure
 *  @sa wispr_portal_web_result_success
 *  @sa wispr_portal_web_result
 *
 *  @private
 *
 */
static void portal_manage_failure_status(
			struct connman_wispr_portal_context *wp_context,
			int err,
			const char *message)
{
	struct connman_service *service = wp_context->service;
	enum connman_ipconfig_type type = wp_context->type;

	wp_context->cb(service, type, false, err, message);
}

/**
 *  @brief
 *    Handle a successful "online" HTTP-based Internet reachability
 *    check.
 *
 *  This handles a successful (that is, completed with a HTTP 200 "OK"
 *  status code) "online" HTTP-based Internet reachability check
 *  previously-initiated with #__connman_wispr_start.
 *
 *  @param[in]      result      A pointer to the mutable HTTP result
 *                              for the successful "online" HTTP-based
 *                              Internet reachability check this is
 *                              handling.
 *  @param[in,out]  wp_context  A pointer to the mutable WISPr portal
 *                              detection context associated with the
 *                              successful "online" HTTP-based
 *                              Internet reachability check this is
 *                              handling.
 *
 *  @sa __connman_wispr_start
 *  @sa wispr_portal_web_result
 *
 */
static void portal_manage_success_status(GWebResult *result,
			struct connman_wispr_portal_context *wp_context)
{
	struct connman_service *service = wp_context->service;
	enum connman_ipconfig_type type = wp_context->type;
	const char *str = NULL;

	DBG("");

	/* We currently don't do anything with this info */
	if (g_web_result_get_header(result, "X-ConnMan-Client-IP",
				&str))
		connman_info("Client-IP: %s", str);

	if (g_web_result_get_header(result, "X-ConnMan-Client-Country",
				&str))
		connman_info("Client-Country: %s", str);

	if (g_web_result_get_header(result, "X-ConnMan-Client-Region",
				&str))
		connman_info("Client-Region: %s", str);

	if (g_web_result_get_header(result, "X-ConnMan-Client-Timezone",
				&str))
		connman_info("Client-Timezone: %s", str);

	wp_context->cb(service, type, true, 0, NULL);
}

static bool wispr_route_request(const char *address, int ai_family,
		int if_index, gpointer user_data)
{
	int result = -1;
	struct connman_wispr_portal_context *wp_context = user_data;
	g_autofree char *interface = NULL;
	const char *gateway;
	struct wispr_route *route;
	uint32_t metric = 0;

	interface = connman_inet_ifname(if_index);

	gateway = __connman_ipconfig_get_gateway_from_index(if_index,
		wp_context->type);

	if (!gateway)
		return false;

	result = __connman_service_get_route_metric(wp_context->service,
				&metric);
	if (result < 0) {
		DBG("failed to get metric for service %p (%s): %s (%d)",
			wp_context->service,
			connman_service_get_identifier(wp_context->service),
			strerror(-result), result);

		return false;
	}

	DBG("add route to %s via %s dev %d (%s) metric %u type %d (%s) "
		"ops %p",
		address,
		gateway,
		if_index, interface,
		metric,
		wp_context->type,
		__connman_ipconfig_type2string(wp_context->type),
		wp_context->ops);

	route = g_try_new0(struct wispr_route, 1);
	if (route == 0) {
		DBG("could not create struct");
		return false;
	}

	result = wp_context->ops->add_host_route_with_metric(
					if_index,
					address,
					gateway,
					metric);
	if (result < 0) {
		g_free(route);
		return false;
	}

	route->address = g_strdup(address);
	route->if_index = if_index;
	route->metric = metric;

	wp_context->route_list = g_slist_prepend(wp_context->route_list, route);

	return true;
}

static void wispr_portal_request_portal(
		struct connman_wispr_portal_context *wp_context)
{
	int err = 0;

	DBG("wispr/portal context %p service %p (%s) type %d (%s)",
		wp_context,
		wp_context->service,
		connman_service_get_identifier(wp_context->service),
		wp_context->type, __connman_ipconfig_type2string(wp_context->type));

	/*
	 * Retain a reference to the WISPr/portal context to account for
	 * 'g_web_request_get' maintaining a weak reference to it. This will
	 * be released in the 'wispr_portal_web_result' callback.
	 */
	wispr_portal_context_ref(wp_context);
	wp_context->request_id = g_web_request_get(wp_context->web,
					wp_context->status_url,
					wispr_portal_web_result,
					wispr_route_request,
					wp_context, &err);

	DBG("wp_context->request_id %d err %d",
		wp_context->request_id, err);

	if (wp_context->request_id == 0) {
		portal_manage_failure_status(wp_context, -err, strerror(-err));
		wispr_portal_error(wp_context);
		wispr_portal_context_unref(wp_context);
	}
}

static bool wispr_input(const guint8 **data, gsize *length,
						gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	GString *buf;
	gsize count;

	DBG("");

	buf = g_string_sized_new(100);

	g_string_append(buf, "button=Login&UserName=");
	g_string_append_uri_escaped(buf, wp_context->wispr_username,
								NULL, FALSE);
	g_string_append(buf, "&Password=");
	g_string_append_uri_escaped(buf, wp_context->wispr_password,
								NULL, FALSE);
	g_string_append(buf, "&FNAME=0&OriginatingServer=");
	g_string_append_uri_escaped(buf, wp_context->status_url, NULL, FALSE);

	count = buf->len;

	g_free(wp_context->wispr_formdata);
	wp_context->wispr_formdata = g_string_free(buf, FALSE);

	*data = (guint8 *) wp_context->wispr_formdata;
	*length = count;

	return false;
}

static void wispr_portal_browser_reply_cb(struct connman_service *service,
					bool authentication_done,
					const char *error, void *user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	struct connman_wispr_portal *wispr_portal;
	int index;

	DBG("");

	if (!service || !wp_context)
		return;

	/*
	 * No way to cancel this if wp_context has been freed, so we lookup
	 * from the service and check that this is still the right context.
	 */
	index = __connman_service_get_index(service);
	if (index < 0)
		return;

	wispr_portal = g_hash_table_lookup(wispr_portal_hash,
					GINT_TO_POINTER(index));
	if (!wispr_portal)
		return;

	if (wp_context != wispr_portal->ipv4_context &&
		wp_context != wispr_portal->ipv6_context)
		return;

	if (!authentication_done) {
		free_wispr_routes(wp_context);
		wispr_portal_error(wp_context);
		wispr_portal_context_unref(wp_context);
		return;
	}

	/* Restarting the test */
	__connman_service_wispr_start(service, wp_context->type);
	wispr_portal_context_unref(wp_context);
}

static void wispr_portal_request_wispr_login(struct connman_service *service,
				bool success,
				const char *ssid, int ssid_len,
				const char *username, const char *password,
				bool wps, const char *wpspin,
				const char *error, void *user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;

	DBG("");

	if (error) {
		if (g_strcmp0(error,
			"net.connman.Agent.Error.LaunchBrowser") == 0) {
			if (__connman_agent_request_browser(service,
					wispr_portal_browser_reply_cb,
					wp_context->redirect_url,
					wp_context) == -EINPROGRESS)
				return;
		}

		wispr_portal_context_unref(wp_context);
		return;
	}

	g_free(wp_context->wispr_username);
	wp_context->wispr_username = g_strdup(username);

	g_free(wp_context->wispr_password);
	wp_context->wispr_password = g_strdup(password);

	wp_context->request_id = g_web_request_post(wp_context->web,
					wp_context->wispr_msg.login_url,
					"application/x-www-form-urlencoded",
					wispr_input, wispr_portal_web_result,
					wp_context, NULL);

	connman_wispr_message_init(&wp_context->wispr_msg);
}

static bool wispr_manage_message(GWebResult *result,
			struct connman_wispr_portal_context *wp_context)
{
	DBG("Message type: %s (%d)",
		message_type_to_string(wp_context->wispr_msg.message_type),
					wp_context->wispr_msg.message_type);
	DBG("Response code: %s (%d)",
		response_code_to_string(wp_context->wispr_msg.response_code),
					wp_context->wispr_msg.response_code);

	if (wp_context->wispr_msg.access_procedure)
		DBG("Access procedure: %s",
			wp_context->wispr_msg.access_procedure);
	if (wp_context->wispr_msg.access_location)
		DBG("Access location: %s",
			wp_context->wispr_msg.access_location);
	if (wp_context->wispr_msg.location_name)
		DBG("Location name: %s",
			wp_context->wispr_msg.location_name);
	if (wp_context->wispr_msg.login_url)
		DBG("Login URL: %s", wp_context->wispr_msg.login_url);
	if (wp_context->wispr_msg.abort_login_url)
		DBG("Abort login URL: %s",
			wp_context->wispr_msg.abort_login_url);
	if (wp_context->wispr_msg.logoff_url)
		DBG("Logoff URL: %s", wp_context->wispr_msg.logoff_url);

	switch (wp_context->wispr_msg.message_type) {
	case 100:
		DBG("Login required");

		wp_context->wispr_result = CONNMAN_WISPR_RESULT_LOGIN;

		wispr_portal_context_ref(wp_context);
		if (__connman_agent_request_login_input(wp_context->service,
					wispr_portal_request_wispr_login,
					wp_context) != -EINPROGRESS) {
			wispr_portal_error(wp_context);
			wispr_portal_context_unref(wp_context);
		} else
			return true;

		break;
	case 120: /* Falling down */
	case 140:
		if (wp_context->wispr_msg.response_code == 50) {
			wp_context->wispr_result = CONNMAN_WISPR_RESULT_ONLINE;

			g_free(wp_context->wispr_username);
			wp_context->wispr_username = NULL;

			g_free(wp_context->wispr_password);
			wp_context->wispr_password = NULL;

			g_free(wp_context->wispr_formdata);
			wp_context->wispr_formdata = NULL;

			wispr_portal_request_portal(wp_context);

			return true;
		} else
			wispr_portal_error(wp_context);

		break;
	default:
		break;
	}

	return false;
}

/**
 *  @brief
 *    Handle closure and finalization of a web request associated with
 *    an unsuccessful "online" HTTP-based Internet reachability check.
 *
 *  @param[in]      error       A pointer to the immutable GLib GError
 *                              instance associated the web request
 *                              failure.
 *  @param[in]      result      A pointer to the mutable web request
 *                              result being finalized.
 *  @param[in,out]  wp_context  A pointer to the mutable WISPr portal
 *                              detection context associated with the
 *                              unsuccessful "online" HTTP-based
 *                              Internet reachability check this is
 *                              finalizing.
 *
 *  @sa wispr_portal_web_result
 *  @sa wispr_portal_web_result_success
 *
 *  @private
 *
 */
static void wispr_portal_web_result_failure(const GError *error,
		GWebResult *result,
		struct connman_wispr_portal_context *wp_context)
{
	portal_manage_failure_status(wp_context, 0, error->message);

	free_wispr_routes(wp_context);
	wp_context->request_id = 0;
}

/**
 *  @brief
 *    Handle closure and finalization of a web request associated with
 *    a successful "online" HTTP-based Internet reachability check.
 *
 *  @param[in]      result      A pointer to the mutable web request
 *                              result being finalized.
 *  @param[in,out]  wp_context  A pointer to the mutable WISPr portal
 *                              detection context associated with the
 *                              successful "online" HTTP-based
 *                              Internet reachability check this is
 *                              finalizing.
 *
 *  @sa wispr_portal_web_result
 *  @sa wispr_portal_web_result_failure
 *
 *  @private
 *
 */
static void wispr_portal_web_result_success(GWebResult *result,
		struct connman_wispr_portal_context *wp_context)
{
	guint16 status;
	const char *str = NULL;
	const char *redirect = NULL;

	status = g_web_result_get_status(result);

	DBG("status: %03u", status);

	switch (status) {
	case GWEB_HTTP_STATUS_CODE_UNKNOWN:
		wispr_portal_context_ref(wp_context);
		__connman_agent_request_browser(wp_context->service,
				wispr_portal_browser_reply_cb,
				wp_context->status_url, wp_context);
		break;
	case GWEB_HTTP_STATUS_CODE_OK:
		if (wp_context->wispr_msg.message_type >= 0)
			break;

		if (g_web_result_get_header(result, "X-ConnMan-Status",
						&str)) {
			portal_manage_success_status(result, wp_context);
		} else {
			wispr_portal_context_ref(wp_context);
			__connman_agent_request_browser(wp_context->service,
					wispr_portal_browser_reply_cb,
					wp_context->redirect_url, wp_context);
		}

		break;
	case GWEB_HTTP_STATUS_CODE_MULTIPLE_CHOICES:
	case GWEB_HTTP_STATUS_CODE_MOVED_PERMANENTLY:
	case GWEB_HTTP_STATUS_CODE_FOUND:
	case GWEB_HTTP_STATUS_CODE_SEE_OTHER:
	case GWEB_HTTP_STATUS_CODE_TEMPORARY_REDIRECT:
	case GWEB_HTTP_STATUS_CODE_PERMANENT_REDIRECT:
		if (!g_web_supports_tls() ||
			!g_web_result_get_header(result, "Location",
							&redirect)) {

			wispr_portal_context_ref(wp_context);
			__connman_agent_request_browser(wp_context->service,
					wispr_portal_browser_reply_cb,
					wp_context->status_url, wp_context);
			break;
		}

		DBG("Redirect URL: %s", redirect);

		wp_context->redirect_url = g_strdup(redirect);

		wispr_portal_context_ref(wp_context);
		wp_context->request_id = g_web_request_get(wp_context->web,
				redirect, wispr_portal_web_result,
				wispr_route_request, wp_context, NULL);

		return;
	case GWEB_HTTP_STATUS_CODE_BAD_REQUEST:
		portal_manage_failure_status(wp_context, 0, "bad request");
		break;

	case GWEB_HTTP_STATUS_CODE_NOT_FOUND:
		portal_manage_failure_status(wp_context, 0, "resource not found");
		break;

	case GWEB_HTTP_STATUS_CODE_REQUEST_TIMEOUT:
		portal_manage_failure_status(wp_context, 0, "request timeout");
		break;

	case GWEB_HTTP_STATUS_CODE_HTTP_VERSION_NOT_SUPPORTED:
		wispr_portal_context_ref(wp_context);
		__connman_agent_request_browser(wp_context->service,
				wispr_portal_browser_reply_cb,
				wp_context->status_url, wp_context);
		break;
	default:
		break;
	}

	free_wispr_routes(wp_context);
	wp_context->request_id = 0;
}

/**
 *  @brief
 *    Handle the receipt of new data and the potential closure and
 *    finalization of a web request.
 *
 *  This handles the receipt of new data associated with @a result and
 *  the potential closure and finalization of it, if appropriate.
 *
 *  @param[in]      error      An optional pointer to the immutable
 *                             GLib GError instance associated the web
 *                             request, if it failed.
 *  @param[in]      result     A pointer to the mutable web request
 *                             result with data to process or to be
 *                             finalized.
 *  @param[in,out]  user_data  A pointer to the mutable WISPr portal
 *                             detection context associated with the
 *                             "online" HTTP-based Internet
 *                             reachability check this is finalizing.
 *
 *  @returns
 *    True if web request should wait for and process further data;
 *    otherwise, false.
 *
 *  @sa wispr_portal_web_result_failure
 *  @sa wispr_portal_web_result_success
 *  @sa wispr_manage_message
 *
 *  @private
 *
 */
static bool wispr_portal_web_result(const GError *error, GWebResult *result,
		gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	const guint8 *chunk = NULL;
	gsize length;

	DBG("error %p result %p user_data %p wispr_result %d",
		error, result, user_data, wp_context->wispr_result);

	if (wp_context->wispr_result != CONNMAN_WISPR_RESULT_ONLINE) {
		g_web_result_get_chunk(result, &chunk, &length);

		DBG("length %zu", length);

		if (length > 0) {
			g_web_parser_feed_data(wp_context->wispr_parser,
								chunk, length);
			/* read more data */
			return true;
		}

		g_web_parser_end_data(wp_context->wispr_parser);

		DBG("wp_context->wispr_msg.message_type %d",
			wp_context->wispr_msg.message_type);

		if (wp_context->wispr_msg.message_type >= 0) {
			if (wispr_manage_message(result, wp_context))
				goto done;
		}
	}

	if (error)
		wispr_portal_web_result_failure(error, result, wp_context);
	else
		wispr_portal_web_result_success(result, wp_context);

done:
	wp_context->wispr_msg.message_type = -1;

	/*
	 * Release a reference to the WISPr/portal context to balance the
	 * earlier reference retained to account for 'g_web_request_get'
	 * maintaining a weak reference to it.
	 */
	wispr_portal_context_unref(wp_context);
	return false;
}

static char *parse_proxy(const char *proxy)
{
	char *proxy_server;

	if (!g_strcmp0(proxy, "DIRECT"))
		return NULL;

	if (!g_str_has_prefix(proxy, "PROXY "))
		return NULL;

	proxy_server = g_strdup(proxy + 6);

	/* Use first proxy server */
	for (char *c = proxy_server; *c != '\0'; ++c) {
		if (*c == ';') {
			*c = '\0';
			break;
		}
	}

	g_strstrip(proxy_server);

	return proxy_server;
}

/**
 *  @brief
 *    Log a Wireless Internet Service Provider roaming (WISPr) proxy
 *    auto-configuration (PAC)-related online reachability check
 *    failure.
 *
 *  @param[in]  wp_context  A pointer to the immutable WISPr/portal
 *                          context for which to log the proxy
 *                          auto-configuration (PAC)-related online
 *                          reachability check failure.
 *  @param[in]  reason      A pointer to an immutable, null-terminated
 *                          C string containing the reason for the
 *                          proxy auto-configuration (PAC)-related
 *                          online reachability check failure which
 *                          will be included at the start of the log
 *                          message.
 *
 *  @private
 *
 */
static void wispr_log_proxy_failure(
		struct connman_wispr_portal_context const *wp_context,
		const char *reason)
{
	g_autofree char *interface = NULL;

	interface = connman_service_get_interface(wp_context->service);

	connman_error("%s with proxy auto-configuration (PAC) URL %s "
			"for %s [ %s ] online check URL %s",
			reason,
			connman_service_get_proxy_url(wp_context->service),
			interface,
			__connman_service_type2string(
				connman_service_get_type(wp_context->service)),
			wp_context->status_url);
}

static void proxy_callback(const char *proxy, void *user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;
	char *proxy_server;

	DBG("wp_context %p proxy %p", wp_context, proxy);

	if (!wp_context)
		return;

	if (!proxy) {
		wispr_log_proxy_failure(wp_context, "No valid proxy");

		portal_manage_failure_status(wp_context, 0, "no valid proxy");

		return;
	}

	DBG("proxy %s", proxy);

	wp_context->proxy_token = 0;

	proxy_server = parse_proxy(proxy);
	if (proxy_server) {
		g_web_set_proxy(wp_context->web, proxy_server);
		g_free(proxy_server);
	}

	g_web_set_accept(wp_context->web, NULL);
	g_web_set_user_agent(wp_context->web, "ConnMan/%s wispr", VERSION);
	g_web_set_close_connection(wp_context->web, TRUE);

	connman_wispr_message_init(&wp_context->wispr_msg);

	wp_context->wispr_parser = g_web_parser_new(
					"<WISPAccessGatewayParam",
					"WISPAccessGatewayParam>",
					xml_wispr_parser_callback, wp_context);

	wispr_portal_request_portal(wp_context);
	wispr_portal_context_unref(wp_context);
}

static gboolean no_proxy_callback(gpointer user_data)
{
	struct connman_wispr_portal_context *wp_context = user_data;

	wp_context->proxy_timeout = 0;

	proxy_callback("DIRECT", wp_context);

	return FALSE;
}

/**
 *  @brief
 *    Start a WISPr / portal detection request / HTTP-based Internet
 *    reachability check for the specified WISPr / portal context.
 *
 *  This attempts to start a WISPr / portal detection request /
 *  HTTP-based Internet reachability check for the network service IP
 *  configuration type associated with the provided WISPr / portal
 *  context with the provided connection timeout.
 *
 *  @param[in,out]  wp_context          A pointer to the mutable WISPr
 *                                      / portal context to supporting
 *                                      running the WISPr request /
 *                                      HTTP-based Internet
 *                                      reachability check.
 *  param[in]       connect_timeout_ms  The time, in milliseconds, for
 *                                      the TCP connection timeout.
 *                                      Connections that take longer
 *                                      than this will be aborted. A
 *                                      value of zero ('0') indicates
 *                                      that no explicit connection
 *                                      timeout will be used, leaving
 *                                      the timeout to the underlying
 *                                      operating system and its
 *                                      network stack.
 *
 *  @retval  0        If the WISPr request / HTTP-based Internet
 *                    reachability check was successfully queued.
 *  @retval  -EINVAL  If the network interface or network interface
 *                    index associated with @a wp_context->service is
 *                    invalid, if @a wp_context->service has no name
 *                    servers, or if a proxy could not be successfully
 *                    associated with @a wp_context->service.
 *  @retval  -ENOMEM  If a GWeb instance could not be allocated for
 *                    the WISPr request / HTTP-based Internet
 *                    reachability check.
 *
 *  @sa __connman_wispr_start
 *
 */
static int wispr_portal_detect(struct connman_wispr_portal_context *wp_context,
						guint connect_timeout_ms)
{
	enum connman_service_proxy_method proxy_method;
	char *interface = NULL;
	char **nameservers = NULL;
	int if_index;
	int err = 0;
	int i;

	DBG("wispr/portal context %p service %p (%s) type %d (%s)",
		wp_context,
		wp_context->service,
		connman_service_get_identifier(wp_context->service),
		wp_context->type, __connman_ipconfig_type2string(wp_context->type));

	interface = connman_service_get_interface(wp_context->service);
	if (!interface)
		return -EINVAL;

	if_index = connman_inet_ifindex(interface);
	if (if_index < 0) {
		DBG("Could not get ifindex");
		err = -EINVAL;
		goto done;
	}

	DBG("index %d (%s)", if_index, interface);

	nameservers = connman_service_get_nameservers(wp_context->service);
	if (!nameservers) {
		DBG("Could not get nameservers");
		err = -EINVAL;
		goto done;
	}

	wp_context->web = g_web_new(if_index);
	if (!wp_context->web) {
		DBG("Could not set up GWeb");
		err = -ENOMEM;
		goto done;
	}

	if (getenv("CONNMAN_WEB_DEBUG"))
		g_web_set_debug(wp_context->web, web_debug, "WEB");

	g_web_set_connect_timeout(wp_context->web, connect_timeout_ms);

	if (wp_context->type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		g_web_set_address_family(wp_context->web, AF_INET);
		wp_context->status_url = online_check_ipv4_url;
	} else {
		g_web_set_address_family(wp_context->web, AF_INET6);
		wp_context->status_url = online_check_ipv6_url;
	}

	for (i = 0; nameservers[i]; i++)
		g_web_add_nameserver(wp_context->web, nameservers[i]);

	proxy_method = connman_service_get_proxy_method(wp_context->service);

	DBG("proxy_method %d", proxy_method);

	/*
	 * Include both CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN and
	 * CONNMAN_SERVICE_PROXY_METHOD_DIRECT in avoiding a call to
	 * connman_proxy_lookup, since the former will always result in
	 * a WISPr request "falling down a hole" that will only ever
	 * result in a failure completion.
	 *
	 * Whether following the connman_proxy_lookup / proxy_callback
	 * path or the g_idle_add / no_proxy_callback path, both funnel to
	 * proxy_callback. Retain a reference to the WISPr/portal context
	 * here and release it there (that is, in proxy_callback) such
	 * that the context is retained while the lookup or the idle
	 * timeout are in flight.
	 */
	if (proxy_method != CONNMAN_SERVICE_PROXY_METHOD_DIRECT &&
			proxy_method != CONNMAN_SERVICE_PROXY_METHOD_UNKNOWN) {
		wispr_portal_context_ref(wp_context);

		wp_context->proxy_token = connman_proxy_lookup(interface,
						wp_context->status_url,
						wp_context->service,
						proxy_callback, wp_context);

		if (wp_context->proxy_token == 0) {
			wispr_log_proxy_failure(wp_context, "Failed to lookup");

			err = -EINVAL;
			wispr_portal_context_unref(wp_context);
		}
	} else if (wp_context->proxy_timeout == 0) {
		wispr_portal_context_ref(wp_context);

		wp_context->proxy_timeout = g_idle_add(no_proxy_callback,
						wp_context);
	}

done:
	g_strfreev(nameservers);

	g_free(interface);
	return err;
}

/**
 *  @brief
 *    Return whether WISPr requests / HTTP-based Internet reachability
 *    checks are supported.
 *
 *  This determines whether WISPr requests / HTTP-based Internet
 *  reachability check are supported for the specified network service
 *  based on its associated technology type.
 *
 *  @param[in]  service  A pointer to the immutable network service
 *                       for which to check WISPr requests / HTTP-based
 *                       Internet reachability check support.
 *
 *  @returns
 *    True if the network service technology type supports WISPr
 *    requests / HTTP-based Internet reachability checks; otherwise,
 *    false.
 *
 */
static bool is_wispr_supported(const struct connman_service *service)
{
	if (!service)
		return false;

	switch (connman_service_get_type(service)) {
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIFI:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return true;
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_P2P:
	default:
		return false;
	}
}

/**
 *  @brief
 *    Start a HTTP-based Internet reachability check for the specified
 *    network service IP configuration type.
 *
 *  This attempts to start a HTTP-based Internet reachability check
 *  for the specified network service IP configuration type with the
 *  provided connection timeout and completion callback.
 *
 *  @param[in,out]  service             A pointer to the mutable network
 *                                      service for which to start the
 *                                      reachability check.
 *  @param[in]      type                The IP configuration type for
 *                                      which the reachability check
 *                                      is to be started.
 *  param[in]       connect_timeout_ms  The time, in milliseconds, for
 *                                      the TCP connection timeout.
 *                                      Connections that take longer
 *                                      than this will be aborted. A
 *                                      value of zero ('0') indicates
 *                                      that no explicit connection
 *                                      timeout will be used, leaving
 *                                      the timeout to the underlying
 *                                      operating system and its
 *                                      network stack.
 *  @param[in]      callback            The callback to be invoked when
 *                                      the reachability check
 *                                      terminates, either on failure
 *                                      or success.
 *
 *  @retval  0            If successful.
 *  @retval  -EINVAL      If the reachability check subsystem is not
 *                        properly initialized, @a callback is null,
 *                        or if the network interface index associated
 *                        with @a service is invalid.
 *  @retval  -EOPNOTSUPP  If a reachability check is not supported for
 *                        the network service technology type.
 *  @retval  -ENOMEM      If resources could not be allocated for the
 *                        reachability check.
 *
 *  @sa __connman_wispr_stop
 *
 */
int __connman_wispr_start(struct connman_service *service,
					enum connman_ipconfig_type type,
					guint connect_timeout_ms,
					__connman_wispr_cb_t callback)
{
	struct connman_wispr_portal_context *wp_context = NULL;
	struct connman_wispr_portal *wispr_portal = NULL;
	int index, err;

	DBG("service %p (%s) type %d (%s) connect_timeout %u ms callback %p",
		service, connman_service_get_identifier(service),
		type, __connman_ipconfig_type2string(type),
		connect_timeout_ms, callback);

	if (!wispr_portal_hash || !callback)
		return -EINVAL;

	if (!is_wispr_supported(service))
		return -EOPNOTSUPP;

	index = __connman_service_get_index(service);
	if (index < 0)
		return -EINVAL;

	wispr_portal = g_hash_table_lookup(wispr_portal_hash,
					GINT_TO_POINTER(index));
	if (!wispr_portal) {
		wispr_portal = g_try_new0(struct connman_wispr_portal, 1);
		if (!wispr_portal)
			return -ENOMEM;

		g_hash_table_replace(wispr_portal_hash,
					GINT_TO_POINTER(index), wispr_portal);
	}

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		wp_context = wispr_portal->ipv4_context;
	else
		wp_context = wispr_portal->ipv6_context;

	/* If there is already an existing context, we wipe it */
	if (wp_context)
		wispr_portal_context_unref(wp_context);

	wp_context = create_wispr_portal_context();
	if (!wp_context) {
		err = -ENOMEM;
		goto free_wp;
	}

	wp_context->service = service;
	wp_context->type = type;
	wp_context->cb = callback;
	wp_context->wispr_portal = wispr_portal;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4) {
		wispr_portal->ipv4_context = wp_context;
		wispr_portal->ipv4_context->ops =
			&ipv4_wispr_portal_context_route_ops;
	} else if (type == CONNMAN_IPCONFIG_TYPE_IPV6) {
		wispr_portal->ipv6_context = wp_context;
		wispr_portal->ipv6_context->ops =
			&ipv6_wispr_portal_context_route_ops;
	} else {
		err = -EINVAL;
		goto free_wp;
	}

	err = wispr_portal_detect(wp_context, connect_timeout_ms);
	if (err)
		goto free_wp;
	return 0;

free_wp:
	DBG("err %d wp_context %p", err, wp_context);

	portal_manage_failure_status(wp_context, -err, strerror(-err));

	g_hash_table_remove(wispr_portal_hash, GINT_TO_POINTER(index));
	return err;
}

/**
 *  @brief
 *    Cancel a HTTP-based Internet reachability check for the specified
 *    network service IP configuration type.
 *
 *  This attempts to cancel a HTTP-based Internet reachability check
 *  for the specified network service IP configuration type.
 *
 *  If a matching HTTP-based Internet reachability check is found, the
 *  original callback specified with #__connman_wispr_start will be
 *  invoked with a success value of zero ('0') or false and an error
 *  value of -ECANCELED.
 *
 *  @param[in,out]  service             A pointer to the mutable network
 *                                      service for which to cancel the
 *                                      reachability check.
 *  @param[in]      type                The IP configuration type for
 *                                      which the reachability check
 *                                      is to be cancelled.
 *
 *  @retval  0            If a matching HTTP-based Internet reachability
 *                        check was successfully cancelled.
 *  @retval  -EINVAL      If the IP configuration type is not X or Y or
 *                        if the network interface index associated
 *                        with @a service is invalid.
 *  @retval  -ENOENT      If a matching HTTP-based Internet reachability
 *                        check could not be found.
 *  @retval  -EOPNOTSUPP  If HTTP-based Internet reachability checks are
 *                        not supported for the technology type
 *                        associated with @a service.
 *
 *  @sa __connman_wispr_start
 *  @sa __connman_wispr_stop
 *
 */
int __connman_wispr_cancel(struct connman_service *service,
					enum connman_ipconfig_type type)
{
	struct connman_wispr_portal_context *wp_context = NULL;
	struct connman_wispr_portal *wispr_portal = NULL;
	int index;

	DBG("service %p (%s) type %d (%s)",
		service, connman_service_get_identifier(service),
		type, __connman_ipconfig_type2string(type));

	if (!is_wispr_supported(service))
		return -EOPNOTSUPP;

	index = __connman_service_get_index(service);
	if (index < 0)
		return -EINVAL;

	DBG("index %d", index);

	if (!wispr_portal_hash)
		return -ENOENT;

	wispr_portal = g_hash_table_lookup(wispr_portal_hash,
					GINT_TO_POINTER(index));

	DBG("wispr_portal %p", wispr_portal);

	if (!wispr_portal)
		return -ENOENT;

	if (type == CONNMAN_IPCONFIG_TYPE_IPV4)
		wp_context = wispr_portal->ipv4_context;
	else if (type == CONNMAN_IPCONFIG_TYPE_IPV6)
		wp_context = wispr_portal->ipv6_context;
	else
		return -EINVAL;

	DBG("wp_context %p", wp_context);

	if (!wp_context)
		return -ENOENT;

	cancel_connman_wispr_portal_context(wp_context);

	portal_manage_failure_status(wp_context, -ECANCELED,
		strerror(ECANCELED));

	wispr_portal_context_unref(wp_context);

	return 0;
}

/**
 *  @brief
 *    Stop all HTTP-based Internet reachability checks for the specified
 *    network service.
 *
 *  This attempts to stop all HTTP-based Internet reachability checks
 *  for the specified network service.
 *
 *  @param[in,out]  service  A pointer to the mutable network service
 *                           for which to stop all reachability
 *                           checks.
 *
 *  @sa __connman_wispr_start
 *  @sa __connman_wispr_cancel
 *
 */
void __connman_wispr_stop(struct connman_service *service)
{
	struct connman_wispr_portal *wispr_portal;
	int index;

	DBG("service %p (%s)", service, connman_service_get_identifier(service));

	if (!wispr_portal_hash)
		return;

	index = __connman_service_get_index(service);
	if (index < 0)
		return;

	wispr_portal = g_hash_table_lookup(wispr_portal_hash,
					GINT_TO_POINTER(index));
	if (!wispr_portal)
		return;

	if ((wispr_portal->ipv4_context &&
	     service == wispr_portal->ipv4_context->service) ||
	    (wispr_portal->ipv6_context &&
	     service == wispr_portal->ipv6_context->service))
		g_hash_table_remove(wispr_portal_hash, GINT_TO_POINTER(index));
}

int __connman_wispr_init(void)
{
	DBG("");

	wispr_portal_hash = g_hash_table_new_full(g_direct_hash,
						g_direct_equal, NULL,
						free_connman_wispr_portal);

	online_check_ipv4_url =
		connman_setting_get_string("OnlineCheckIPv4URL");
	online_check_ipv6_url =
		connman_setting_get_string("OnlineCheckIPv6URL");

	return 0;
}

void __connman_wispr_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(wispr_portal_hash);
	wispr_portal_hash = NULL;
}
