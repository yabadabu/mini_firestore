#include <cassert>
#include <cstdio>
#include "mini_firestore/mini_firestore.h"
#include "demo_credentials.h"

using namespace MiniFireStore;

struct Person {
    int age = 32;
    std::string name = "john";
    Person() = default;
    Person(int new_age, const std::string& new_name) : age( new_age ), name( new_name ) {}
    bool operator==( const Person& s ) const {
        return age == s.age && name == s.name;
    }
};

void to_json(json& j, const Person& p) {
    j = json{{"name", p.name}, {"age", p.age}};
}

void from_json(const json& j, Person& p) {
    p.name = j.value("name", p.name );
    p.age = j.value("age", p.age );
}

struct School {
    std::vector< Person > population;
    std::string           city;
    int                   age = 100;
    double                ratio = 0.5f;
    Person                director;
    bool                  is_local = false;
    bool                  is_private = true;
    bool operator==( const School& s ) const {
        return age == s.age && 
            city == s.city && 
            population == s.population && 
            director == s.director && 
            ratio == s.ratio &&
            is_local == s.is_local &&
            is_private == s.is_private
            ;
    }
};

void to_json(json& j, const School& p) {
    j = json{
        {"city", p.city}, 
        {"age", p.age}, 
        {"ratio", p.ratio}, 
        {"director", p.director}, 
        {"population", p.population},
        {"is_local", p.is_local},
        {"is_private", p.is_private},
    };
}

void from_json(const json& j, School& p) {
    p.city = j.value("city", p.city );
    p.age = j.value("age", p.age );
    p.ratio = j.value("ratio", p.ratio );
    p.director = j.value("director", p.director );
    p.population = j.value("population", p.population );
    p.is_local = j.value("is_local", p.is_local );
    p.is_private = j.value("is_private", p.is_private );
}

void testDelete(MiniFireStore::Firestore& db) {
    printf( "test Delete begins...\n");
    Ref r = db.ref( "free/James" );
    Person p( 99, "James");
    printf( "Writing item\n");
    r.write( p, [=]( const Result& result ) {
        assert( !result.err );
        printf( "Deleting item\n");
        r.del( [=]( const Result& result ) {
            assert( !result.err );
            printf( "Reading it again\n" );
            r.read( [=]( const Result& result ) {
                printf( "Read result %d %s\n", result.err, result.j.dump().c_str() );
                assert( result.err == ERR_DOC_MISSING );
                Person pr;
                if( result.get( pr )) {
                    printf( "The item is still in the db!\n");
                } else {
                    printf( "The item is no longer in the db!\n");
                }
                printf( "test Delete ends...\n");
            });
        });
    });

    while( !db.hasFinished() ) db.update();
}

School initSchool() {
    // Create a local obj, including 
    School school;
    school.age = 150;
    school.ratio = 0.8f;
    school.city = "Barcelona";
    school.director = { 80, "Sr. Director" };
    school.is_local = true;
    school.is_private = false;
    school.population.emplace_back( 20, "John");
    school.population.emplace_back( 19, "Peter");
    school.population.emplace_back( 15, "Alex");
    return school;
}

void testReadWriteDelete(MiniFireStore::Firestore& db) {

    School school = initSchool();
    
    Ref r = db.ref( "free" );
    printf( "testReadWriteDelete begins...\n");
    printf( "Adding new item\n");
    r.add( school, [=]( const Result& result ) {
        assert( !result.err );
        
        Ref s = r.child( result.added_id );
        printf( "NewID: %s\n", s.id().c_str());

        printf( "Reading added item\n");
        s.read([=]( const Result& result ) {
            School read_school;
            if( !result.get(read_school) )
                return;
            bool equal = (read_school == school);
            printf( "Equal: %d\n", equal);
            assert( equal );

            printf( "Changing data\n");
            School school2 = school;
            school2.city = "Girona";
            school2.age = 250;
            school2.ratio = 0.3f;
            s.write( school2, [=](const Result& result ) {

                printf( "Reading back changed data\n");
                s.read([=]( const Result& result ) {
                    School read_school2;
                    if( !result.get(read_school2) )
                        return;
                    bool equal = (read_school2 == school2);
                    bool diff = !(read_school2 == school);
                    assert( equal );
                    assert( diff );

                    printf( "Deleting created doc\n");
                    s.del( [=]( const Result& result ) {
                        assert( !result.err );
                        printf( "Reading deleted data\n");

                        s.read([=]( const Result& result ) {
                            printf( "Read %s\n", result.str.c_str());


                            printf( "testReadWriteDelete ends...\n");
                        });

                    });

                });

            });

        });
    });
  
    while( !db.hasFinished() ) db.update();
}

void testSubCollections( Firestore& db ) {

    Ref ref = db.ref("users").child(db.uid());
    int ncompletes = 0;
    Person person(30, "Sr. Smith");
    // ref needs to be passed by value or it will go out of scope!
    ref.write( person, [&,ref]( Result& r ) {
        printf( "%s Saved\n", ref.path().c_str() );

        Ref my_msgs = ref.child( "connections" );

        auto report_done = [&,my_msgs]( Result& r ) {
            ++ncompletes;
            printf( "Connections added %d\n", ncompletes );
            if( ncompletes == 4 ) {

                my_msgs.query( Query(), [](Result& r ) {
                    assert( !r.err );
                    assert( r.j.is_array() );
                    std::vector< Person > people;
                    if( !r.get( people ))
                      return;
                    printf( "It has %ld connections registered!\n", people.size());
                });
            }
        };

        Person f1(24, "Adam");
        Person f2(25, "Berta");
        Person f3(22, "Charles");
        Person f4(42, "Dickens");
        my_msgs.add( f1, report_done );
        my_msgs.add( f2, report_done );
        my_msgs.add( f3, report_done );
        my_msgs.add( f4, report_done );
    });

    while( !db.hasFinished() ) db.update();
}

void testQuery(MiniFireStore::Firestore& db) {
    
    Person people[5] = {
     Person( 30, "John-30"),
     Person( 40, "Mary-40"),
     Person( 20, "Alex-20"),
     Person( 25, "Peter-25"),
     Person( 50, "Ander-50"),
    };
    std::vector< std::string > ids;

    Ref r = db.ref("free");

    // Add some elements
    if( 0 ) {
        for( auto p : people ) {
            r.add( p, [&]( const Result& res ) {
                std::string new_id = res.added_id;
                ids.push_back( new_id );
                printf( "NewID: %s\n", new_id.c_str());
            });
        }
    }

    auto checkQuery = [](const Result& result, int expected_result, const char* title) {
        assert( !result.err );
        for( auto& jp : result.j ) {
            Person rp = jp.get<Person>();
            std::string id = jp.value( "id", "");
            printf("  [%s] Age:%d Name:%s  [ID:%s]\n", title, rp.age, rp.name.c_str(), id.c_str());
        }
        if( expected_result >= 0 ) {
            assert(result.j.size() == expected_result);
        }
    };

    // Run Basic query with basic filter
    {
        printf( "People age > 25\n");
        Query q;
        q.conditions.emplace_back( "age", Condition::GreaterThan, 25 );
        r.query( q, [&](const Result& result) {
            checkQuery( result, 3, "age > 25" );
        });
    }

    // Run Basic query
    {
        printf( "People age >= 25\n");
        Query q;
        q.conditions.emplace_back( "age", Condition::GreaterThanOrEqual, 25 );
        r.query( q, [&](const Result& result) {
            checkQuery( result, 4, "age >= 25" );
        });
    }

    // Double condition Basic query
    {
        printf( "People age >= 25 and age < 45n\n");        
        Query q;
        q.conditions.emplace_back( "age", Condition::GreaterThanOrEqual, 25 );
        q.conditions.emplace_back( "age", Condition::LessThan, 45 );
        r.query( q, [&](const Result& result) {
            checkQuery( result, 3, "age >=25 && < 45");
        });
    }

    {
        printf( "People limit to 2\n");        
        Query q;
        q.conditions.emplace_back( "age", Condition::GreaterThan, 0 );
        q.order_by.emplace_back( "age", Query::ASCENDING );
        printf( "Ascending\n");
        r.query( q, [&](const Result& result) {
            checkQuery( result, 5, "Ascending");
        });
        printf( "Descending\n");
        q.order_by[0].direction = Query::DESCENDING;
        r.query( q, [&](const Result& result) {
            checkQuery( result, 5, "Descending");
        });

        printf( "Descending Limited to 3\n");
        q.order_by[0].direction = Query::DESCENDING;
        q.limit = 3;
        r.query( q, [&](const Result& result) {
            checkQuery( result, 3, "Desc limit 3");
        });

        // printf( "Descending Limited to 3 starting at 3\n");
        // q.order_by[0].direction = Query::DESCENDING;
        // q.limit = 3;
        // q.first = 3;
        // db.query( "free", q, [&](const Result& result) {
        //     checkQuery( result, 2);
        // });
    }
    
    printf( "Query Tests ok\n");
    while( !db.hasFinished() ) db.update();
}

void testInc(MiniFireStore::Firestore& db) {
    Ref ref = db.ref( "users" ).child( db.uid() );

    School p = initSchool();
    int delta = 5;
    ref.write( p, [=]( Result& r ) {
        printf( "Wrote school! >%s<\n", r.str.c_str());
        // Increment a field of a suboject in the same doc
        ref.inc( "director.age", delta, [=](Result& r) {
            printf( "Incremented by %d! >%s<\n", delta, r.str.c_str());
            ref.read( [=]( Result& r ) {
                printf( "read back!\n");
                assert( !r.err );
                School np;
                if( r.get(np) ) {
                    printf( "Check %d+%d == %d\n", p.director.age, delta, np.director.age );
                    assert( p.director.age + delta == np.director.age );
                }
            });
        });
    });

    while( !db.hasFinished() ) db.update();
}

class MySample {
public:
    void myLog( MiniFireStore::eLevel level, const char* msg ) {
        printf( "[%d] %s", level, msg );
    }
};

int main(int argc, char** argv)
{
    MiniFireStore::globalInit();

    MySample s;
    MiniFireStore::setLogCallback(std::bind(&MySample::myLog, &s, std::placeholders::_1, std::placeholders::_2));
    MiniFireStore::setLogLevel( eLevel::Log );

    Firestore db;
    db.configure(db_name, api_key);  // Defined in demo_credentials.h
    db.connect(user_email, user_password, [&](Result& result) {
        if (result.err) {
            printf("Login failed: %s\n", result.j.dump().c_str());
            return;
        }
        testInc(db);
        testSubCollections(db);
        testDelete(db);
        testReadWriteDelete(db);
        testQuery(db);
        });

    while( !db.hasFinished() ) {
        db.update();
    }

    printf("Ending\n");

    MiniFireStore::globalCleanup();
	return 0;
}
