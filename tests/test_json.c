/* sce::Json (src/runtime_json.c) as MAXIBOOST ON reads its server configuration: an object of
 * unsigned numbers, strings and arrays of objects, read through the C++ entry points. */
#include "../src/runtime_json.c"
#include <assert.h>

static void test_configuration(void) {
    const char text[]="{\"version\":3,\"maintenance\":0,\"information_url\":\"https://x/\\u00e9\","
                      "\"dlc_schedule\":[{\"id\":1,\"start_at\":\"2020-07-30T00:00:00Z\",\"end_at\":\"2099-01-01T00:00:00Z\"},"
                      "{\"id\":2,\"start_at\":\"2020-08-01T00:00:00Z\"}],\"neg\":-5,\"real\":1.5,\"ok\":true,\"none\":null}";
    Value root; value_init(&root);
    assert(parse(&root,text,sizeof(text)+16)==0); /* a NUL ends the text early */
    assert(value_type(&root)==J_OBJECT);
    assert(value_type(value_member(&root,"version"))==J_UINTEGER && *value_unsigned(value_member(&root,"version"))==3);
    assert(value_type(value_member(&root,"missing"))==J_NULL);
    assert(value_type(value_member(&root,"neg"))==J_INTEGER && *value_integer(value_member(&root,"neg"))==-5);
    assert(value_type(value_member(&root,"real"))==J_REAL);
    assert(value_type(value_member(&root,"ok"))==J_BOOLEAN && value_type(value_member(&root,"none"))==J_NULL);
    const Handle *url=value_string(value_member(&root,"information_url"));
    assert(!strcmp(string_c_str(url),"https://x/\xc3\xa9") && string_length(url)==12);
    /* The schedule as guest+0xee920 walks it: copy the array, iterate, copy each element. */
    Handle array; array_copy_init(&array,value_array(value_member(&root,"dlc_schedule")));
    assert(array_size(&array)==2);
    Iterator it, end; array_begin(&it,&array);
    Value element; value_init(&element);
    int seen=0;
    for (;; iterator_next(&it)) {
        array_end(&end,&array);
        const int more=iterator_differ(&it,&end);
        iterator_destroy(&end);
        if (!more) break;
        value_assign_op(&element,iterator_value(&it));
        assert(*value_unsigned(value_member(&element,"id"))==(uint64_t)(seen+1));
        Handle date; string_init(&date);
        assert(value_to_string(value_member(&element,"start_at"),&date)==0);
        assert(!strncmp(string_c_str(&date),"2020-0",6));
        string_destroy(&date);
        ++seen;
    }
    assert(seen==2);
    iterator_destroy(&it);
    value_destroy(&element);
    array_destroy(&array);
    /* Copies share; the original outlives a cleared copy. */
    Value copy; value_copy_init(&copy,&root);
    value_clear(&copy);
    assert(*value_unsigned(value_member(&root,"version"))==3);
    Handle json; string_init(&json);
    value_to_string(value_member(&root,"dlc_schedule"),&json);
    assert(!strncmp(string_c_str(&json),"[{\"id\":1,",9));
    string_destroy(&json);
    value_destroy(&root);
    /* Errors leave the value as it was. */
    Value bad; value_init(&bad);
    assert(parse(&bad,"{\"a\":",5)!=0 && value_type(&bad)==J_NULL);
    assert(parse(&bad,"[1,2] x",7)!=0);
    assert(value_type(value_array(&bad)==&empty_array.self ? &bad : &bad)==J_NULL);
}

int main(void) {
    test_configuration();
    puts("json tests passed");
    return 0;
}
