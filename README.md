This is a library for making web servers in C.

Features:
* HTTP/1.0 support
* HTTP/1.1 support
* privilage dropping
* timeouts

Shortcomings:
* Will only give things in the text/html encoding
* `Transfer-Encoding: Chunked` is not supported, but it is not required by either supported HTTP spec.

To use:
* `#include "cwebserv.h"` in the appropriate C source file
* run this function (in the same file):
```
void cwebserv(char *port,
        char *(*makeoutput)(char *query, char *path, int *final_length));
/*Start the web server on port *port. Run the function makeoutput for each
  request.*/
```
example:
```
#include "cwebserv.h"

char *makeoutput(char *query, char *path, int *final_length) {
    char *template = "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Query and Path</title>\n"
        "</head>\n"
        "<body>\n"
        "<p>Your HTTP path was \"%s\".</p>\n"
        "<p>Your HTTP query was \"%s\".</p>\n"
        "</body>\n"
        "</html>\n";
    int length = sizeof(template) + strlen(query) + strlen(path);
    char *ret = malloc(maxlen);
    /*final_length needs to be set to the length of the returned string, not
        including the final null terminator, which can sometimes be accomplisted
        using strlen.*/
    final_length = snprintf(ret, maxlen, template, path, query);
    return ret;
}

int main() {
    cwebserv("8080" /*or whatever port it will run on*/, generate);
}
```
You also get these definitions, which might be useful, but probably won't be:
```
#define BUFSTEP 1000 //buffer allocation increment
#define BUFSTART 5000 //initial buffer allocation
#define INITIAL_FD_LENGTH 100 //initial file descriptor allocation
#define FD_LENGTH_INCREMENT 100 //file descriptor allocation increment
#define DROP_TO_GROUP 32766
#define DROP_TO_USER 32767
```

If this documentation doesn't make sense, that is a bug.
