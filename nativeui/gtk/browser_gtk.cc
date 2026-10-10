// Copyright 2017 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/browser.h"

#include <JavaScriptCore/JavaScript.h>
#include <webkit2/webkit2.h>

#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "nativeui/app.h"
#include "nativeui/gtk/nu_protocol_stream.h"
#include "nativeui/state.h"
#include "nativeui/gtk/util/widget_util.h"

namespace nu {

namespace {

const char* kIgnoreNextFinish = "ignore-next-finish";

// Stores the protocol factories.
using ProtocolHandlerMap = std::map<std::string, Browser::ProtocolHandler>;
ProtocolHandlerMap& GetProtocolHandlers() {
  static base::NoDestructor<ProtocolHandlerMap> handlers;
  return *handlers;
}

std::string JSStringToString(JSStringRef js) {
  size_t max_size = JSStringGetMaximumUTF8CStringSize(js);
  std::string str(max_size, '\0');
  JSStringGetUTF8CString(js, &str[0], max_size);
  size_t length = strlen(str.c_str());
  str.erase(length, max_size - length);
  return str;
}

base::Value JSResultToBaseValue(WebKitJavascriptResult* js_result) {
  auto* context = webkit_javascript_result_get_global_context(js_result);
  auto* value = webkit_javascript_result_get_value(js_result);
  // TODO(zcbenz): Convert types directly instead of using JSON parsing.
  JSStringRef json = JSValueCreateJSONString(context, value, 0, nullptr);
  if (!json)
    return base::Value();
  std::string json_str = JSStringToString(json);
  JSStringRelease(json);
  std::optional<base::Value> result = base::JSONReader::Read(json_str);
  if (!result)
    return base::Value();
  return std::move(*result);
}

gboolean OnContextMenu(WebKitWebView* widget,
                       GtkWidget *default_menu,
                       WebKitHitTestResult* hit_test_result,
                       gboolean triggered_with_keyboard,
                       Browser* view) {
  return TRUE;
}

void OnNotifyTitle(WebKitWebView*, GParamSpec*, Browser* view) {
  view->on_update_title.Emit(view, view->GetTitle());
}

void OnNotifyIsLoading(WebKitWebView*, GParamSpec*, Browser* view) {
  view->on_change_loading.Emit(view);
}

void OnClose(WebKitWebView*, Browser* view) {
  view->on_close.Emit(view);
}

void OnLoadChanged(WebKitWebView* widget, WebKitLoadEvent event,
                   Browser* view) {
  switch (event) {
    case WEBKIT_LOAD_STARTED:
      view->on_start_navigation.Emit(view, webkit_web_view_get_uri(widget));
      break;
    case WEBKIT_LOAD_COMMITTED:
      view->on_commit_navigation.Emit(view, webkit_web_view_get_uri(widget));
      break;
    case WEBKIT_LOAD_FINISHED:
      // Do not emit when navigation fails.
      if (g_object_get_data(G_OBJECT(widget), kIgnoreNextFinish)) {
        g_object_set_data(G_OBJECT(widget), kIgnoreNextFinish, nullptr);
        break;
      }
      view->on_finish_navigation.Emit(view, webkit_web_view_get_uri(widget));
      break;
    case WEBKIT_LOAD_REDIRECTED:
      break;
  }
}

gboolean OnLoadFailed(WebKitWebView* widget, WebKitLoadEvent event,
                      const gchar* url, GError* error, Browser* view) {
  view->on_fail_navigation.Emit(view, url, error->code);
  // WebKitGTK always emits "finished" event even when navigation fails, so we
  // need to ignore the next "finished" event.
  g_object_set_data(G_OBJECT(widget), kIgnoreNextFinish, view);
  return TRUE;
}

void OnBackForwadListChanged(WebKitBackForwardList* backforward_list,
                             WebKitBackForwardListItem* added,
                             GList* removed,
                             Browser *view) {
  view->on_update_command.Emit(view);
}

void OnJavaScriptFinish(WebKitWebView* webview,
                        GAsyncResult* result,
                        Browser::ExecutionCallback* callback) {
  auto* js_result = webkit_web_view_run_javascript_finish(
      webview, result, nullptr);
  if (*callback) {
    if (js_result)
      (*callback)(true, JSResultToBaseValue(js_result));
    else
      (*callback)(false, base::Value());
  }
  if (js_result)
    webkit_javascript_result_unref(js_result);
  delete callback;
}

void OnGetCookiesFinish(WebKitCookieManager* cookie_manager,
                        GAsyncResult* result,
                        Browser::CookiesCallback* callback) {
  GList* cookies = webkit_cookie_manager_get_cookies_finish(
      cookie_manager, result, nullptr);
  // WebKitGTK 表示空 cookie 列表为 NULL，是合法查询结果：URL 无 cookie
  // 时不崩溃，按空列表回调（下方循环与 g_list_free_full 均接受 NULL）。
  std::vector<Cookie> ret;
  for (GList* l = cookies; l; l = l->next) {
    SoupCookie* cookie = static_cast<SoupCookie*>(l->data);
    ret.push_back({
      soup_cookie_get_name(cookie),
      soup_cookie_get_value(cookie),
      soup_cookie_get_domain(cookie),
      soup_cookie_get_path(cookie),
      static_cast<bool>(soup_cookie_get_http_only(cookie)),
      static_cast<bool>(soup_cookie_get_secure(cookie)),
    });
  }
  (*callback)(std::move(ret));
  g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
  delete callback;
}

void OnScriptMessage(WebKitUserContentManager* manager,
                     WebKitJavascriptResult* js_result,
                     Browser* browser) {
  if (browser->stop_serving() || !js_result)
    return;
  auto* context = webkit_javascript_result_get_global_context(js_result);
  auto* value = webkit_javascript_result_get_value(js_result);
  if (!JSValueIsString(context, value))
    return;
  JSStringRef str = JSValueToStringCopy(context, value, nullptr);
  browser->InvokeBindings(JSStringToString(str));
  JSStringRelease(str);
}

void OnNullProtocolRequest(WebKitURISchemeRequest* request, gpointer) {
  GError* error = g_error_new_literal(
      g_quark_from_static_string("yue"),
      WEBKIT_NETWORK_ERROR_UNKNOWN_PROTOCOL,
      "The protocol has been unregistered");
  webkit_uri_scheme_request_finish_error(request, error);
  g_error_free(error);
}

void OnProtocolRequest(WebKitURISchemeRequest* request,
                       Browser::ProtocolHandler* handler) {
  // Create job.
  ProtocolJob* protocol_job =
      (*handler)(webkit_uri_scheme_request_get_uri(request));
  if (!protocol_job) {
    GError* error = g_error_new_literal(
        g_quark_from_static_string("yue"),
        WEBKIT_NETWORK_ERROR_FAILED,
        "The protocol handler did not return a request job");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }
  // Manage the protocol_job with the stream.
  // DO NOT pass protocol_job to the lambda, it would cause circular ref.
  GInputStream* protocol_stream = nu_protocol_stream_new(protocol_job);
  std::string mime_type;
  protocol_job->GetMimeType(&mime_type);
  // Start.
  g_object_ref(request);
  protocol_job->Plug([protocol_stream, request, mime_type](int size) {
    webkit_uri_scheme_request_finish(
        request, protocol_stream, size,
        mime_type.empty() ? nullptr : mime_type.c_str());
    g_object_unref(protocol_stream);
    g_object_unref(request);
  });
  if (!protocol_job->Start()) {
    GError* error = g_error_new_literal(
        g_quark_from_static_string("yue"),
        WEBKIT_NETWORK_ERROR_FAILED,
        "The protocol request job failed to start");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    // Free on failure.
    g_object_unref(protocol_stream);
    g_object_unref(request);
  }
}

// Session profiles: browsers created with the same profile share one web
// context, and hence one session (cookies, local storage, cache), while
// browsers without a profile keep using the process-wide default context.
using ProfileContextMap = std::map<std::string, WebKitWebContext*>;
ProfileContextMap& GetProfileContexts() {
  static base::NoDestructor<ProfileContextMap> contexts;
  return *contexts;
}

// Registers a known protocol handler on a context. The handler map owns the
// handlers, so no deleter is given to WebKit.
void RegisterProtocolOnContext(WebKitWebContext* context,
                              const std::string& scheme) {
  auto it = GetProtocolHandlers().find(scheme);
  if (it == GetProtocolHandlers().end())
    return;
  webkit_web_context_register_uri_scheme(
      context,
      scheme.c_str(),
      reinterpret_cast<WebKitURISchemeRequestCallback>(&OnProtocolRequest),
      &it->second,
      nullptr);
}

// <user data dir>/yue-profiles/<app>/<profile>: the application name keeps the
// profiles of unrelated programs apart.
std::string ProfileBaseDir(const std::string& profile) {
  std::string app;
  // The state is created before any view, the program name is only a fallback
  // for profiles asked for very early.
  if (State::GetCurrent())
    app = App::GetCurrent()->GetID();
  if (app.empty()) {
    const char* name = g_get_prgname();
    app = (name && *name) ? name : "yue";
  }
  gchar* dir = g_build_filename(g_get_user_data_dir(), "yue-profiles",
                                app.c_str(), profile.c_str(), nullptr);
  std::string result(dir);
  g_free(dir);
  return result;
}

WebKitWebContext* GetOrCreateProfileContext(const std::string& profile) {
  auto it = GetProfileContexts().find(profile);
  if (it != GetProfileContexts().end())
    return it->second;
  // The data and cache directories carry the isolation: WebKit stores the
  // cookies, local storage and cache of a context under them.
  std::string base_dir = ProfileBaseDir(profile);
  std::string data_dir = base_dir + "/data";
  std::string cache_dir = base_dir + "/cache";
  g_mkdir_with_parents(data_dir.c_str(), 0700);
  g_mkdir_with_parents(cache_dir.c_str(), 0700);
  WebKitWebsiteDataManager* manager = webkit_website_data_manager_new(
      "base-data-directory", data_dir.c_str(),
      "base-cache-directory", cache_dir.c_str(),
      nullptr);
  WebKitWebContext* context =
      webkit_web_context_new_with_website_data_manager(manager);
  g_object_unref(manager);
  // Protocols are registered per context, so a new context catches up with the
  // schemes registered so far.
  for (const auto& handler : GetProtocolHandlers())
    RegisterProtocolOnContext(context, handler.first);
  GetProfileContexts()[profile] = context;
  return context;
}

}  // namespace

void Browser::PlatformInit(Options options) {
  // Install native bindings script.
  WebKitUserContentManager* manager = webkit_user_content_manager_new();
  g_signal_connect(manager, "script-message-received::yue",
                   G_CALLBACK(OnScriptMessage), this);
  webkit_user_content_manager_register_script_message_handler(manager, "yue");

  // Create webview: a browser with a profile gets its own context, and hence
  // its own session, while the others keep sharing the default context.
  GtkWidget* webview = nullptr;
  if (options.profile.empty()) {
    webview = webkit_web_view_new_with_user_content_manager(manager);
  } else {
    webview = GTK_WIDGET(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "web-context", GetOrCreateProfileContext(options.profile),
                     "user-content-manager", manager,
                     nullptr));
  }
  TakeOverView(webview);

  // Assign a new settings to avoid affecting other webviews.
  WebKitSettings* settings = webkit_settings_new();
  // Configurations.
  if (options.devtools) {
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, true);
    webkit_settings_set_enable_developer_extras(settings, true);
  }
  if (options.allow_file_access_from_files) {
    webkit_settings_set_allow_file_access_from_file_urls(settings, true);
  }
  if (!options.hardware_acceleration) {
    webkit_settings_set_hardware_acceleration_policy(
        settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
  }
  webkit_web_view_set_settings(WEBKIT_WEB_VIEW(webview), settings);

  // Disable the context menu.
  if (!options.context_menu) {
    g_signal_connect(webview, "context-menu", G_CALLBACK(OnContextMenu), this);
  }

  // Install events.
  g_signal_connect(webview, "notify::title", G_CALLBACK(OnNotifyTitle), this);
  g_signal_connect(webview, "notify::is-loading",
                   G_CALLBACK(OnNotifyIsLoading), this);
  g_signal_connect(webview, "close", G_CALLBACK(OnClose), this);
  g_signal_connect(webview, "load-changed", G_CALLBACK(OnLoadChanged), this);
  g_signal_connect(webview, "load-failed", G_CALLBACK(OnLoadFailed), this);
  WebKitBackForwardList* backforward_list =
      webkit_web_view_get_back_forward_list(WEBKIT_WEB_VIEW(webview));
  g_signal_connect(backforward_list, "changed",
                   G_CALLBACK(OnBackForwadListChanged), this);
}

void Browser::PlatformDestroy() {
  WebKitUserContentManager* manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(GetNative()));
  g_object_unref(manager);
}

void Browser::LoadURL(const std::string& url) {
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(GetNative()), url.c_str());
}

void Browser::LoadHTML(const std::string& str,
                       const std::string& base_url) {
  webkit_web_view_load_html(WEBKIT_WEB_VIEW(GetNative()),
                            str.c_str(), base_url.c_str());
}

std::string Browser::GetURL() {
  const char* url = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(GetNative()));
  if (url)
    return url;
  else
    return std::string();
}

std::string Browser::GetTitle() {
  const char* title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(GetNative()));
  if (title)
    return title;
  else
    return std::string();
}

void Browser::SetUserAgent(const std::string& user_agent) {
  auto* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(GetNative()));
  webkit_settings_set_user_agent(settings, user_agent.c_str());
}

void Browser::ExecuteJavaScript(const std::string& code,
                                const ExecutionCallback& callback) {
  webkit_web_view_run_javascript(
      WEBKIT_WEB_VIEW(GetNative()),
      code.c_str(),
      nullptr,
      reinterpret_cast<GAsyncReadyCallback>(&OnJavaScriptFinish),
      new ExecutionCallback(callback));
}

void Browser::GetCookiesForURL(const std::string& url,
                               const CookiesCallback& callback) {
  if (!callback)
    return;
  WebKitWebContext* context =
      webkit_web_view_get_context(WEBKIT_WEB_VIEW(GetNative()));
  WebKitCookieManager* cookie_manager =
      webkit_web_context_get_cookie_manager(context);
  webkit_cookie_manager_get_cookies(cookie_manager, url.c_str(),
      nullptr,
      reinterpret_cast<GAsyncReadyCallback>(&OnGetCookiesFinish),
      new CookiesCallback(callback));
}

void Browser::GoBack() {
  webkit_web_view_go_back(WEBKIT_WEB_VIEW(GetNative()));
}

bool Browser::CanGoBack() const {
  return webkit_web_view_can_go_back(WEBKIT_WEB_VIEW(GetNative()));
}

void Browser::GoForward() {
  webkit_web_view_go_forward(WEBKIT_WEB_VIEW(GetNative()));
}

bool Browser::CanGoForward() const {
  return webkit_web_view_can_go_forward(WEBKIT_WEB_VIEW(GetNative()));
}

void Browser::Reload() {
  webkit_web_view_reload(WEBKIT_WEB_VIEW(GetNative()));
}

void Browser::Stop() {
  webkit_web_view_stop_loading(WEBKIT_WEB_VIEW(GetNative()));
}

bool Browser::IsLoading() const {
  return webkit_web_view_is_loading(WEBKIT_WEB_VIEW(GetNative()));
}

void Browser::SetBackgroundColor(Color color) {
  View::SetBackgroundColor(color);
  GdkRGBA gdkcolor = color.ToGdkRGBA();
  webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(GetNative()), &gdkcolor);
}

void Browser::PlatformUpdateScripts() {
  WebKitUserContentManager* manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(GetNative()));
  // WebKit has no API to update a single script, so the list is rebuilt as a
  // whole — which is also what keeps the user scripts alive across binding
  // updates.
  webkit_user_content_manager_remove_all_scripts(manager);
  auto add_script = [manager](const std::string& code,
                              WebKitUserScriptInjectionTime injection_time,
                              bool main_frame_only) {
    WebKitUserScript* script = webkit_user_script_new(
        code.c_str(),
        main_frame_only ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
                        : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        injection_time,
        nullptr,
        nullptr);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
  };
  // The binding script goes first so that the user scripts can use the
  // bindings, and user scripts are injected in the order they were added.
  add_script(GetBindingScript(), WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
             true);
  for (const UserScript& user_script : user_scripts()) {
    add_script(user_script.code,
               user_script.timing == UserScriptInjectionTime::kDocumentStart
                   ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                   : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
               user_script.main_frame_only);
  }
}

// static
bool Browser::RegisterProtocol(const std::string& scheme,
                               ProtocolHandler handler) {
  GetProtocolHandlers()[scheme] = std::move(handler);
  // Protocols are registered per context, so the scheme goes to the default
  // context and to every profile created so far.
  RegisterProtocolOnContext(webkit_web_context_get_default(), scheme);
  for (const auto& context : GetProfileContexts())
    RegisterProtocolOnContext(context.second, scheme);
  return true;
}

// static
void Browser::UnregisterProtocol(const std::string& scheme) {
  GetProtocolHandlers().erase(scheme);
  // There is no unregister API, just replace with a handler to return error.
  auto unregister_on = [&scheme](WebKitWebContext* context) {
    webkit_web_context_register_uri_scheme(
        context, scheme.c_str(), &OnNullProtocolRequest, nullptr, nullptr);
  };
  unregister_on(webkit_web_context_get_default());
  for (const auto& context : GetProfileContexts())
    unregister_on(context.second);
}

}  // namespace nu
