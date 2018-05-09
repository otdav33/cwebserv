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
