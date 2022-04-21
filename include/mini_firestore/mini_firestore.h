#pragma once

#include <string>
#include <functional>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

struct curl_slist;

namespace MiniFireStore
{

  struct Result;
  class Firestore;
  using Callback = std::function<void(Result& j)>;

  static const int ERR_DOC_MISSING = 1;
  static const int ERR_AUTH_EMAIL_NOT_FOUND = 400;

  struct Condition {
    std::string field_name;
    enum Operator { Equal, NotEqual, GreaterThan, GreaterThanOrEqual, LessThan, LessThanOrEqual, ArrayContains, ArrayContainsAny, In, NotIn };
    Operator    op;
    json        ref_value;
    Condition(const std::string& new_field_name, Operator new_op, const json& new_ref_value)
      : field_name(new_field_name)
      , op(new_op)
      , ref_value(new_ref_value)
    {};
  };

  struct Query {
    std::vector< Condition > conditions;
    int first = 0;
    int limit = -1;

    enum eDirection { ASCENDING, DESCENDING };

    struct OrderBy {
      std::string field_name;
      eDirection  direction = Query::ASCENDING;
      OrderBy() = default;
      OrderBy(const std::string& new_field_name, eDirection new_direction)
        : field_name(new_field_name)
        , direction(new_direction)
      { }
    };
    std::vector< OrderBy > order_by;
  };

  class Ref {
  public:

    uint32_t read(Callback cb) const;
    uint32_t write(const json& j, Callback cb) const;
    uint32_t del(Callback cb) const;
    uint32_t add(const json& j, Callback cb) const;
    uint32_t query(const Query& q, Callback cb) const;
    uint32_t inc(const std::string& field_name, double value, Callback cb) const;
    uint32_t list(Callback cb) const;
    uint32_t patch(const std::string& field_name, const json& new_value, Callback cb) const;

    Ref() = default;

    Ref(Firestore* new_db, const std::string& new_doc_id)
      : db(new_db)
      , doc_id(new_doc_id)
    { }

    std::string id() const;
    const std::string path() const { return doc_id; }
    Ref child(const std::string& subpath) const;

  private:

    Firestore* db = nullptr;
    std::string  doc_id;

    bool sendRPC(const char* url_suffix, const json& body, Result& result, const char* label, int flags = 0) const;
  };

  struct Result {
    int         err = -1;
    std::string str;
    json        j;
    std::string added_id;

    template< typename T >
    bool get(T& obj) const {
      if (err)
        return false;
      obj = j.get<T>();
      return true;
    }

  };

  class Firestore {

  public:

    Firestore() = default;
    Firestore(const Firestore&) = delete;
    ~Firestore() {
      disconnect();
    }

    void configure(const char* project_id, const char* api_key);

    void signUp(const char* email, const char* password, Callback cb);
    void connect(const char* email, const char* password, Callback cb);
    void disconnect();

    bool update();
    bool hasFinished() const;
    void dump() const;

    const std::string& uid() const { return user_id; }
    Ref ref(const std::string& path);

    friend class Ref;

  private:

    void setToken(const std::string& new_token);
    void authRequest(const char* url_base, const char* email, const char* password, Callback cb);

    std::string user_id;
    std::string api_key;
    std::string project_id;
    std::string url_root;
    std::string doc_root;
    std::string token;

    struct OTFRequests;
    OTFRequests* otf = nullptr;

    uint32_t allocRequest(const std::string& url_suffix, const json& jbody, Callback cb, const char* label, int flags = 0);
  };

  // -------------------------------------------------------
  bool globalInit();
  void globalCleanup();

  // -------------------------------------------------------
  enum eLevel {
    Error, Log, Trace,
  };
  using LogCallback = std::function<void(eLevel verbosity, const char* msg)>;
  void setLogCallback(LogCallback new_callback);
  void setLogLevel(eLevel new_level);

  // -------------------------------------------------------
  json timeToISO8601(time_t utc_time);
  bool ISO8601ToTime(const json& j, time_t* out_time_t);
}
