#include <cstdio>
#include <cstdarg>
#include <ctime>
#include "mini_firestore.h"

extern "C" {
#include <curl/curl.h>
}

// Windows specifics
#ifdef _WIN32

#undef min
#define vsnprintf _vsnprintf_s
#define sscanf sscanf_s
struct tm* gmtime_r(const time_t* timer, struct tm* user_tm) {
  gmtime_s(user_tm, timer);
  return user_tm;
}
struct tm* localtime_r(const time_t* timer, struct tm* user_tm) {
  localtime_s(user_tm, timer);
  return user_tm;
}
#endif

namespace MiniFireStore
{
  namespace Ctes {
    const char* api_verify_password_host = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword";
    const char* api_signup_host = "https://identitytoolkit.googleapis.com/v1/accounts:signUp";
    const char* api_firestore_url = "https://firestore.googleapis.com/v1/projects/";
    const char* auth_bearer = "Authorization: Bearer ";         // <-- Has already a space in the right
    const char* json_content_header = "Content-Type: application/json";
    const std::string json_doc_id_key = "_doc_id";
    //const char* client_header = "x-firebase-client";
  }

  static const int RPC_FLAG_DELETE = 1;
  static const int RPC_FLAG_TRACE = 2;
  static const int RPC_FLAG_CONNECT = 4;
  static const int RPC_FLAG_GET = 8;
  static const int RPC_FLAG_PATCH = 16;

  const char* conditionOperatorName(Condition::Operator op) {
    switch (op) {
    case Condition::Equal: return "EQUAL";
    case Condition::NotEqual: return "NOT_EQUAL";
    case Condition::GreaterThan: return "GREATER_THAN";
    case Condition::GreaterThanOrEqual: return "GREATER_THAN_OR_EQUAL";
    case Condition::LessThan: return "LESS_THAN";
    case Condition::LessThanOrEqual: return "LESS_THAN_OR_EQUAL";
    case Condition::ArrayContains: return "ARRAY_CONTAINS";
    case Condition::ArrayContainsAny: return "ARRAY_CONTAINS_ANY";
    case Condition::In: return "IN";
    case Condition::NotIn: return "NOT_IN";
    }
    assert(!"Invalid operator");
    return "EQUAL";
  }

  // -----------------------------------------
  static LogCallback current_callback;
  static eLevel      current_level = eLevel::Error;
  static bool        check_certificates = false;
  static bool        full_curl_traces = false;

  void setLogCallback(LogCallback new_callback) {
    current_callback = new_callback;
  }

  void setLogLevel(eLevel new_level) {
    current_level = new_level;
  }

  void log(eLevel level, const char* fmt, ...) {
    if (!current_callback || level > current_level)
      return;
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    if (n < 0)
      buf[sizeof(buf) - 1] = 0x00;
    va_end(ap);
    current_callback(level, buf);
  }

  // -----------------------------------------
  struct Request;
  static CURL* CurlPrepareRequest(Request* r, curl_slist* chunk);

  // -----------------------------------------
  struct Request {
    uint32_t    req_unique_id = 0;          // Automatically generated sequence
    Result      result;
    std::string url;

    std::string str_recv;
    std::string str_sent;
    size_t      send_offset = 0;

    const char* label = nullptr;            // Pure constant for debug
    int         flags = 0;
    Callback    callback;
  };

  // This class is private of the Firestore OTF = On The Fly Requests
  struct Firestore::OTFRequests {
    std::unordered_map< CURL*, Request* > on_the_fly_request;
    std::vector< Request* > free_requests;
    uint32_t                next_request_unique_id = 0;
    CURLM* multi_handle = nullptr;

    curl_slist* common_chunk = nullptr;
    curl_slist* login_chunk = nullptr;

    OTFRequests() {
      multi_handle = curl_multi_init();
      login_chunk = curl_slist_append(nullptr, Ctes::json_content_header);
    }

    ~OTFRequests() {

      for (auto it : on_the_fly_request)
        unregisterRequest(it.first, it.second);
      on_the_fly_request.clear();

      for (auto r : free_requests)
        delete r;
      free_requests.clear();

      curl_multi_cleanup(multi_handle);

      if (common_chunk)
        curl_slist_free_all(common_chunk);

      curl_slist_free_all(login_chunk);
    }

    void setToken(const std::string& new_token) {
      std::string auth_header = Ctes::auth_bearer + new_token;
      // The headers are shared between all calls.
      if (common_chunk)
        curl_slist_free_all(common_chunk);
      common_chunk = nullptr;
      common_chunk = curl_slist_append(common_chunk, Ctes::json_content_header);
      common_chunk = curl_slist_append(common_chunk, auth_header.c_str());
    }

    Request* newRequest() {
      Request* r = nullptr;
      // take one from the free_requests or create a new one
      if (!free_requests.empty()) {
        r = free_requests.back();
        free_requests.pop_back();
      }
      else {
        r = new Request();
        log(eLevel::Trace, "[%p] alloc new", r);
      }
      // Request and result have the same unique id
      r->req_unique_id = ++next_request_unique_id;
      r->result.req_unique_id = r->req_unique_id;
      return r;
    }

    void registerRequest(Request* r) {

      // Prepare the curl request and add it to the async api
      CURL* curl = CurlPrepareRequest(r, (r->flags & RPC_FLAG_CONNECT) ? login_chunk : common_chunk);
      assert(curl);

      // move it to on_the_fly_request
      on_the_fly_request[curl] = r;

      curl_multi_add_handle(multi_handle, curl);
    }

    void unregisterRequest(CURL* curl, Request* r) {
      assert(curl);
      assert(r);

      // Now we can reuse the request
      free_requests.push_back(r);

      log(eLevel::Trace, "[%p] returns to the pool (now %ld)", r, free_requests.size());

      curl_multi_remove_handle(multi_handle, curl);

      curl_easy_cleanup(curl);
    }

    bool update() {
      assert(multi_handle);

      int num_handles = (int)on_the_fly_request.size();
      CURLMcode rc = curl_multi_perform(multi_handle, &num_handles);
      if (rc) {
        log(eLevel::Error, "curl_multi_perform() failed, code %d.", (int)rc);
        return false;
      }

      bool work_done = false;

      struct CURLMsg* m = nullptr;
      do {
        int msgq = 0;
        m = curl_multi_info_read(multi_handle, &msgq);
        if (m && (m->msg == CURLMSG_DONE)) {

          CURL* curl = m->easy_handle;
          assert(curl);

          auto it = on_the_fly_request.find(curl);
          assert(it != on_the_fly_request.end());

          // Dispatch the callback
          Request* r = it->second;
          assert(r);

          log(eLevel::Trace, "[%p] Request #%d(%s) completes", r, r->req_unique_id, r->label);
          log(eLevel::Trace, "%s", r->str_recv.c_str());

          bool error_detected = r->str_recv.empty();
          if (!error_detected) {
            // Parse the results back to json
            json& j = r->result.j;
            j = json::parse(r->str_recv, nullptr, false);
            if (j.is_discarded()) {
              error_detected = true;
            }
            else {
              error_detected = j.contains("error") || (j.is_array() && j[0].contains("error"));
            }
          }

          // Check for obvious errors
          if (error_detected) {
            log(eLevel::Error, "%s(%s,%s) Err: %s", r->label, r->url.c_str(), r->str_sent.c_str(), r->str_recv.c_str());
            r->result.err = -1;
          }
          else {
            r->result.err = 0;
          }

          // If we are inside a callback waiting to fs to finish, this request is no longer on the fly
          on_the_fly_request.erase(it);

          // Move the recv str to the result object
          r->result.str = std::move(r->str_recv);

          r->callback(r->result);

          unregisterRequest(curl, r);

          work_done = true;
        }
      } while (m);

      return work_done;
    }

  };

  uint32_t Firestore::allocRequest(const std::string& url_suffix, const json& jbody, Callback callback, const char* label, int flags) {

    assert(label);
    if (!otf) {
      log(eLevel::Error, "Not connected");
      return 0;
    }

    Request* r = otf->newRequest();

    // Create the full url, except for the CONNECT request
    std::string url;
    if ((flags & RPC_FLAG_CONNECT) == 0) {
      url = url_root;
      if (url_suffix[0] != ':')
        url.append("/");
    }
    url.append(url_suffix);

    // init
    r->result = Result();
    r->url = url;
    r->str_recv.clear();
    if( !jbody.is_null() )
      r->str_sent = jbody.dump((flags & RPC_FLAG_TRACE) ? 2 : 0, ' ');
    else
      r->str_sent.clear();
    r->send_offset = 0;
    r->label = label;
    r->flags = flags;
    r->callback = callback;

    log(eLevel::Trace, "[%p] Request added #%d (%s)", r, r->req_unique_id, label);
    otf->registerRequest(r);

    return r->req_unique_id;
  }

  bool Firestore::hasFinished() const {
    return otf && otf->on_the_fly_request.empty();
  }

  void Firestore::dump() const {
    if (otf) {
      for (auto it : otf->on_the_fly_request) {
        Request* r = it.second;
        log(eLevel::Log, "[%p] %s %s", r, r->label, r->url.c_str());
      }
    }
  }

  bool Firestore::update() {
    return otf && otf->update();
  }

  static size_t CurlAppendToRequest(char* buffer, size_t size, size_t nitems, void* userdata) {
    Request* r = (Request*)userdata;
    assert(r);
    size_t num_bytes = size * nitems;
    r->str_recv.append(buffer, num_bytes);
    //log(eLevel::Trace, "[%p] Recv body of %ld bytes. New total %ld", r, num_bytes, r->str_recv.length());
    //log(eLevel::Trace, "%s", r->str_recv.c_str());
    return num_bytes;
  }

  static size_t CurlReadFromRequest(char* buffer, size_t size, size_t nitems, void* userdata) {
    Request* r = (Request*)userdata;
    assert(r);
    size_t num_bytes = size * nitems;
    size_t remaining_bytes = r->str_sent.length() - r->send_offset;
    size_t bytes_to_send = std::min(num_bytes, remaining_bytes);
    //log(eLevel::Trace, "[%p] Request body of %ld bytes from %ld -> %ld sent", r, num_bytes, r->send_offset, bytes_to_send);
    memcpy(buffer, r->str_sent.data() + r->send_offset, bytes_to_send);
    r->send_offset += bytes_to_send;
    return bytes_to_send;
  }

  static CURL* CurlPrepareRequest(Request* r, curl_slist* chunk) {
    assert(r);

    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, r->url.c_str());

    if (full_curl_traces)
        r->flags |= RPC_FLAG_TRACE;
    if (r->flags & RPC_FLAG_DELETE) {
      if (r->flags & RPC_FLAG_TRACE)
        log(eLevel::Log, "Custom request delete");
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // Not sending any type of client-id identification
    // ...

    // This is failing in windows
    if (!check_certificates)
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);

    if (r->flags & RPC_FLAG_TRACE) {
      log(eLevel::Log, "URL:%s", r->url.c_str());
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    }

    if (!r->str_sent.empty()) {
      // Convert json to string
      if (r->flags & RPC_FLAG_TRACE)
        log(eLevel::Log, "BODY:%s", r->str_sent.c_str());
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, &CurlReadFromRequest);
      curl_easy_setopt(curl, CURLOPT_READDATA, r);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, r->str_sent.size());
    }

    if (r->flags & RPC_FLAG_GET) {
      curl_easy_setopt(curl, CURLOPT_POST, 0L);
    }

    if (r->flags & RPC_FLAG_PATCH) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlAppendToRequest);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    return curl;
  }

  // ------------------------------------------------------------
  json timeToISO8601(time_t utc_time) {
    char buf[sizeof "2011-10-08T07:07:09.000Z"];
    struct tm tm_buf;
    strftime(buf, sizeof buf, "%FT%TZ", gmtime_r(&utc_time, &tm_buf));
    return buf;
  }

  bool ISO8601ToTime(const json& j, time_t* out_time_t) {
    if (!j.is_string() || !out_time_t)
      return false;
    std::string str = j.get< std::string >();
    if (str.empty())
      return false;

    int msec = 0;   // Not currently used
    struct tm tm = { 0 };
    int n = sscanf(str.c_str(), "%04d-%02d-%02dT%02d:%02d:%02d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &msec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    if (n != 6)
      return false;
    tm.tm_isdst = 0;
    time_t time_stamp = mktime(&tm);

    // Substract the timezone
    struct tm tm_a;
    struct tm tm_b;
    time_t t0 = 1000000;
    struct tm* gmt = gmtime_r(&t0, &tm_a);
    struct tm* gml = localtime_r(&t0, &tm_b);
    int delta = (gml->tm_hour - gmt->tm_hour);
    time_stamp += delta * 3600;
    *out_time_t = time_stamp;
    return true;
  }

  // Does it look like a timestamp?
  // 2022-04-15T14:25:30Z
  bool isTimeISO8601(const std::string& str) {
    if (str.length() >= 20) {
      if (str[4] == '-' && str[7] == '-' && str[10] == 'T'
        && str[13] == ':' && str[16] == ':' && str.back() == 'Z') {
        // Check are all others are digits?
        return true;
      }
    }
    return false;
  }

  // ------------------------------------------------------------
  bool globalInit() {
    // In windows, this will init the winsock stuff
    CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
    return rc == CURLcode::CURLE_OK;
  }

  void globalCleanup() {
    curl_global_cleanup();
  }

  json fromFields(const json& j);
  json fromValue(const json& j);
  json asValue(const json& inValue);
  json asDocument(const json& inDoc);

  json asValue(const json& inValue) {
    if (inValue.is_string()) {
      std::string str = inValue.get<std::string>();

      if (isTimeISO8601(str))
        return { { "timestampValue", str } };

      return { { "stringValue", str } };
    }
    if (inValue.is_boolean()) {
      return { { "booleanValue", inValue.get<bool>() } };
    }
    if (inValue.is_array()) {
      json outValue = json::value_t::array;
      for (const json& j : inValue) {
        outValue.push_back(asValue(j));
      }
      return { { "arrayValue", {{ "values", outValue }}} };
    }
    if (inValue.is_object()) {
      return { { "mapValue", asDocument(inValue) } };
    }
    if (inValue.is_number()) {
      return { { "doubleValue", inValue } };
    }
    if (inValue.is_null()) {
      return { { "nullValue", nullptr } };
    }
    // if( inValue....() ) {
    //     return {{ "bytesValue", nullptr }};
    // }
    return json();
  }

  json asDocument(const json& inDoc) {
    json jDoc = { { "fields", json::value_t::object } };
    for (auto& el : inDoc.items()) {
      jDoc["fields"][el.key()] = asValue(el.value());
    }
    return jDoc;
  }

  json fromFields(const json& j) {
    json outValue = json::value_t::object;
    for (auto& el : j["fields"].items()) {
      outValue[el.key()] = fromValue(el.value());
    }
    return outValue;
  }

  json fromValue(const json& j) {
    json outValue;
    if (j.contains("fields")) {
      return fromFields(j);

    }
    else if (j.contains("mapValue")) {
      return fromFields(j["mapValue"]);

    }
    else if (j.contains("stringValue")) {
      return j["stringValue"];

    }
    else if (j.contains("booleanValue")) {
      return j["booleanValue"];

    }
    else if (j.contains("timestampValue")) {
      return j["timestampValue"];

    }
    else if (j.contains("arrayValue")) {
      outValue = json::value_t::array;
      const json& jarrayValue = j["arrayValue"];
      if (jarrayValue.contains("values")) {
        const json& values = jarrayValue["values"];
        for (const json& el : values)
          outValue.push_back(fromValue(el));
      }

    }
    else if (j.contains("doubleValue")) {
      return j["doubleValue"];

    }
    else if (j.contains("integerValue")) {
      const std::string& integer_str = j["integerValue"].get<std::string>();
      return atoi(integer_str.c_str());
    }
    return outValue;
  }

  static std::string idFromPath(const std::string& path) {
    std::string::size_type pos = path.rfind('/');
    if (pos != std::string::npos)
      return path.substr(pos + 1);
    assert(!"failed to find / in path");
    return path;
  }

  void splitParentAndId(const std::string& full_path, std::string& path, std::string& id) {
    std::string::size_type pos = full_path.rfind('/');
    if (pos != std::string::npos) {
      path = full_path.substr(0, pos);
      id = full_path.substr(pos + 1);
    }
    else {
      id = full_path;
      path.clear();
    }
  }

  void Firestore::configure(const char* new_project_id, const char* new_api_key) {
    project_id = new_project_id;
    url_root = Ctes::api_firestore_url;
    url_root.append(project_id);
    url_root.append("/databases/(default)/documents");
    doc_root = "projects/" + project_id + "/databases/(default)/documents/";
    api_key = new_api_key;
    if (!otf)
      otf = new OTFRequests();
  }

  void Firestore::disconnect() {
    if (otf)
      delete otf;
    otf = nullptr;
    token.clear();
    user_id.clear();
  }

  void Firestore::signUp(const std::string& email, const std::string& password, Callback cb) {
    authRequest(Ctes::api_signup_host, email, password, cb);
  }

  void Firestore::connect(const std::string& email, const std::string& password, Callback cb) {
    authRequest(Ctes::api_verify_password_host, email, password, cb);
  }

  void Firestore::connectOrSignUp(const std::string& email, const std::string& password, Callback cb) {
    connect(email, password, [=](Result& r) {
      if (r.err == ERR_AUTH_EMAIL_NOT_FOUND) {
        log(eLevel::Log, "Email not found. Signing up");
        signUp(email, password, cb);
        return;
      }
      cb(r);
    });
  }

  void Firestore::authRequest(const char* url_base, const std::string& email, const std::string& password, Callback cb) {
    assert(otf);

    std::string url = url_base;
    url.append("?key=");
    url.append(api_key);

    json j = {
        {"email", email},
        {"password", password },
        {"returnSecureToken", true}
    };

    auto pre_cb = [=](Result& result) {
      if (!result.err) {
        user_id = result.j.value("localId", "");
        std::string new_token = result.j.value("idToken", "");
        log(eLevel::Log, "Local UID: %s", user_id.c_str());
        setToken(new_token);
      }
      else {
        if (result.j.contains("error")) {
          result.err = result.j["error"]["code"].get<int>();
        }
      }
      cb(result);
    };

    allocRequest(url, j, pre_cb, "connect", RPC_FLAG_CONNECT);
  }

  void Firestore::setToken(const std::string& new_token) {
    log(eLevel::Log, "Token: %s", new_token.c_str());
    token = new_token;
    otf->setToken(new_token);
  }

  Ref Firestore::ref(const std::string& path) {
    return Ref(this, path);
  }

  Ref Ref::child(const std::string& subpath) const {
    assert(!subpath.empty());
    return Ref(db, doc_id + "/" + subpath);
  }

  std::string Ref::id() const {
    return idFromPath(doc_id);
  }

  // --------------------------------------------------------------------------------
  uint32_t Ref::read(Callback cb) const
  {
    json jbody = { {"documents", { db->doc_root + doc_id }} };

    auto pre_cb = [=](Result& result) {
      if (!result.err) {
        assert(result.j.is_array());
        const json j0 = std::move(result.j[0]);

        if (j0.contains("found")) {
          result.j = fromValue(j0["found"]);
        }
        else if (j0.contains("missing")) {
          result.err = ERR_DOC_MISSING;
          result.j = json::value_t::object;
        }
      }
      cb(result);
    };

    return db->allocRequest(":batchGet", jbody, pre_cb, "read", 0);
  }

  uint32_t Ref::del(Callback cb) const {
    return db->allocRequest(doc_id, nullptr, cb, "del", RPC_FLAG_DELETE);
  }

  uint32_t Ref::add(const json& j, Callback cb) const {
    auto pre_cb = [=](Result& result) {
      if (!result.err) {
        assert(result.j.contains("name"));
        result.added_id = idFromPath(result.j["name"]);
      }
      cb(result);
    };
    return db->allocRequest(doc_id, asDocument(j), pre_cb, "add");
  }

  uint32_t Ref::write(const json& j, Callback cb) const {
    json jDocument = asDocument(j);
    jDocument["name"] = db->doc_root + doc_id;
    json jCmd = {
        { "writes", {{"update", jDocument }} }
    };
    return db->allocRequest(":commit", jCmd, cb, "write");
  }

  uint32_t Ref::inc(const std::string& field_name, double value, Callback cb) const {
    json jCmd = {
        { "writes", {
            {
                {"transform",
                    {
                        {"document", db->doc_root + doc_id},
                        {"fieldTransforms", {
                            {
                                { "fieldPath", field_name },
                                { "increment",
                                    { {"doubleValue", value } }
                                }
                            }
                        }}
                    }
                },
            },
        }}
    };
    auto pre_cb = [=](Result& result) {
      // Transform the result into something more easy to parse for the end-user
      if (!result.err) {
        // Just tripple check each blind access
        assert(result.j.contains("writeResults"));
        assert(result.j["writeResults"].is_array());
        assert(result.j["writeResults"].size() == 1);
        assert(result.j["writeResults"][0].contains("transformResults"));
        assert(result.j["writeResults"][0]["transformResults"].is_array());
        assert(result.j["writeResults"][0]["transformResults"].size() == 1);
        result.j = fromValue(result.j["writeResults"][0]["transformResults"][0]);
      }
      cb(result);
    };
    return db->allocRequest(":commit", jCmd, pre_cb, "inc");
  }

  uint32_t Ref::list(Callback cb) const {
    return db->allocRequest(doc_id, nullptr, cb, "list", RPC_FLAG_GET);
  }

  uint32_t Ref::patch(const std::string& field_name, const json& new_value, Callback cb) const {
    std::string url = doc_id + "?updateMask.fieldPaths=" + field_name + "&mask.fieldPaths=" + field_name;
    json j = { { field_name, new_value } };
    return db->allocRequest(url, asDocument(j), cb, "patch", RPC_FLAG_PATCH);
  }

  // Helpers to convert a OrderBy/Condition to json
  static void to_json(json& j, const Query::OrderBy& order) {
    j = {
        { "field", {
            { "fieldPath", order.field_name }
        }},
        { "direction", order.direction == Query::DESCENDING ? "DESCENDING" : "ASCENDING" }
    };
  }

  static void to_json(json& j, const Condition& cond) {
    j = {
        {"fieldFilter", {
            {"field", {
                { "fieldPath", cond.field_name }
            }},
            {"op", conditionOperatorName(cond.op)},
            {"value", asValue(cond.ref_value)}
        }
    } };
  }

  uint32_t Ref::query(const Query& query, Callback cb) const {

    std::string parent, collection_id;
    splitParentAndId(doc_id, parent, collection_id);

    json jq = {
        { "structuredQuery", {
            { "from", {
                { "collectionId", collection_id }
            }}
        }}
    };
    json& sq = jq["structuredQuery"];

    jq["parent"] = db->doc_root + parent;

    if (!query.conditions.empty()) {
      sq["where"] = { { "compositeFilter", {
          {"filters", query.conditions},
          {"op", "AND"}
      }} };
    }

    if (!query.order_by.empty())
      sq["orderBy"] = query.order_by;

    // if( query.first > 0 )
    //      sq["first"] = query.first;

    if (query.limit > 0)
      sq["limit"] = query.limit;

    auto pre_cb = [=](Result& result) {
      if (!result.err && result.j.is_array() && result.j.size() >= 0) {
        const json j = std::move(result.j);
        result.j = json::value_t::array;
        for (auto& el : j) {
          if (el.contains("document")) {
            const json& jdoc = el["document"];
            result.j.push_back(fromValue(jdoc));
            
            // The doc_id is stored in a member 
            result.j.back()[Ctes::json_doc_id_key] = idFromPath(jdoc["name"]);
          }
        }
      }
      cb(result);
    };

    return db->allocRequest(parent + ":runQuery", jq, pre_cb, "query");
  }

  const std::string& Result::getDocKeyName() {
    return Ctes::json_doc_id_key;
  }


}
