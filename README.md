# mini_firestore
Access firestore using REST api without the official cpp sdk.

# Install

Add the **src/mini_firestore.cpp** file to your project, and include the **mini_firestore/mini_firestore.h** in your headers.
To compile, the nlohmann headers must be accessible (included in the repo)
You also need to link with the curllib (windows version included)
Sorry, no fancy CMakefile this time.

The library has been tested in Windows and OSX

# Usage

## Initialization

libCurl requires a global initialization to be called. If you are not already using libCurl, you should call globalInit()/globalCleanup() to perform this initialization.

## Connecting

Before accessing the db, you need to setup the database and connect with a user/password. You will need
the database name and the API key. You also need to configure the database to accept the email/pass
login method as an authentication method in the firebase administration console. Right now it's the only supported method.

```cpp
    Firestore db;
    db.configure( "YOUR_DATABASE_NAME", "YOUR_API_KEY" );
    const char* email = "user_email";
    const char* password = "secret_password"; 

    db.connect(email, password, [&](Result& result) {
      if( result.err ) { 
        // Handle error, result.j.dump() contains all the details.
        return;
      }
      // You are connected!!
      // ..
    });
```

You should have a Firestore db per connection, beware the Ref objects hold a pointer to the db they belong to.

**None of the methods will block**. But you need to periodically call the **db.update()** to check if any of the tasks have finished and dispatch the callbacks.

## Ref's

A Ref object it's a std::string representing a path in the db, and a pointer to the db object itself.

You apply the read/write/query/del/add methods to operate in that path of the db. The results are returned to you in
the provided callback in a object of type **Result**. The result object is only valid during the lifetime of the lambda.

**Beware of the capturing by reference variables created in the stack!!**

The callback is executed from the same thread which calls the **db.update()** method.

Refs are created directly from the db object, or as child of another Ref.

## Result

The Result object returned on each callback contains:

- **int err** member to indicate if there was some type of error. 
- **std::string str** containing the full text returned by the firestore db
- **json j** The json member from parsing the str answer.
- **std::string added_id** the identifier assigned to new entries when using the **add** method.

The helper method **get** checks if no error was produced and converts the json to the given type. This works as long as the type has the conversions from json already supported using the nlohmann json api.

## Reading a doc

Given a ref, use the **read** method to read the current contents at the reference.

```cpp

  Ref ref = db.Ref( "users").child( db.uid() );
  ref.read([](Result& r) {
    Person person;
    if( !r.get(person) )
    	return;
    // Do something with person
  }
```

## Creating new documents

```cpp
  Ref ref = fb.ref("users").child(db.uid());
  Person person( 40, "John Smith");
  ref.add( person, []( Result& r ) {
    // r.added_id contains the unique identifier of the document assigned by firestore
  });	
```

## Writing doc

```cpp
  Person person( 40, "John Smith");
  ref.write( person, []( Result& r ) {
    // The full contents of the document will be updated with the given object
  });	
```

## Delete 

```cpp
  ref.del( []( Result& r ) {
    // The document at ref has been deleted 
  });	
```

## Queries

The query will return an array of all the documents matching the selected filters. The **Query** object is a struct representing the conditions, sort mode and limits. Beware that some filters require an index to be created in the firestore console.

```cpp
  Query q;
  q.conditions.emplace_back( "age", Condition::GreaterThan, 25 );
  ref.query( q, []( Result& r ) {
    std::vector< Person > people;
    if( !r.get( people ))
      return;
  });	
```

You can also process individual members returned by the query iterating over the **json j** member.

## Log support

You can hook to log/error/trace events using the **setLogCallback** and **setLogLevel**.

The argument is a **std::function<void(MiniFireStore::eLevel level, const char* msg)>**, which you can bind a static function or a member function with a bit more of code.

```cpp
class MySample {
public:
  void myLog( MiniFireStore::eLevel level, const char* msg ) const {
    printf( "[%d] %s", level, msg );
  }
};

...
  // Bind to a member function
  MySample s;
  MiniFireStore::setLogCallback(std::bind(&MySample::myLog, &s, std::placeholders::_1, std::placeholders::_2));
```

# Features
- [x] Authentication using email/pass
- [x] Full read/write/del/add
- [x] Queries with filters
- [x] Async callbacks on top of async curl.
- [x] Automatic (de)serialization using nlohmann json

# Dependencies
- libcurl (https://curl.se/libcurl)
- nlohmann json (https://github.com/nlohmann/json)

# Missing
- [ ] Support for date, ref, binary data types
- [ ] Queries with startAt
- [ ] Better tests
- [ ] Rest of auth methods
- [ ] Make easier support other json libs

No realtime, or offline support is expected.

