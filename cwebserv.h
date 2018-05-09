#include <time.h>
#define BACKLOG 5
#define BUFSTEP 1000 //buffer allocation increment
#define BUFSTART 5000 //initial buffer allocation
#define INITIAL_FD_LENGTH 100 //initial file descriptor allocation
#define FD_LENGTH_INCREMENT 100 //file descriptor allocation increment
#define DROP_TO_GROUP 32766
#define DROP_TO_USER 32767

void cwebserv(char *port,
        char *(*makeoutput)(char *query, char *path, int *final_length));
/*Start the web server on port *port. Run the function makeoutput for each
  request.*/

struct chrbuf {
	//the buffer for incoming socket data, and associated info
	char *buf;
	char *buf_to_send;
	int bsize;
	int buflen;
	int sentbytes;
	int started_sending; //0 for no, 1 for yes
	int finished_sending; //0 for no, 1 for yes, 2 to close the connection
        int keepalive;
        time_t last_accessed;
};
