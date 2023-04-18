#include <stdio.h>
#include <signal.h>

#include "csapp.h"

typedef struct web_object_t
{
  char *response_ptr;
  char path[MAXLINE];
  int content_length;
  struct web_object_t *left, *right;
} web_object_t;

void *thread(void *vargp);
void doit(int clientfd);
void read_requesthdrs(rio_t *rp, void *buf, int serverfd, char *hostname, char *port);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *hostname, char *port, char *path);
web_object_t *find_cache(char *path);
void read_cache(web_object_t *web_object);
void write_cache(web_object_t *web_object);
void send_cache(web_object_t *web_object, int clientfd);

web_object_t *rootp; // 캐시 리스트의 루트
web_object_t *lastp;
int total_cache_size = 0;
static const int is_local_test = 1; // 로컬이 아닌 외부에서 테스트하기 위한 상수 (0 할당 시 도메인과 포트가 고정되어 외부에서 접속 가능)

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv)
{
  signal(SIGPIPE, SIG_IGN);
  rootp = (web_object_t *)calloc(1, sizeof(web_object_t));
  int listenfd, *clientfd;
  char client_hostname[MAXLINE], client_port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  pthread_t tid;

  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]); // 전달받은 포트 번호를 사용해 수신 소켓 생성
  while (1)
  {
    clientlen = sizeof(clientaddr);
    clientfd = Malloc(sizeof(int));
    *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // 클라이언트 연결 요청 수신
    Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
    Pthread_create(&tid, NULL, thread, clientfd);
  }
}

void *thread(void *vargp)
{
  int clientfd = *((int *)vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  doit(clientfd);
  Close(clientfd);
  return NULL;
}

void doit(int clientfd)
{
  int serverfd;
  char request_buf[MAXLINE], response_buf[MAXLINE];
  char method[MAXLINE], uri[MAXLINE], path[MAXLINE], hostname[MAXLINE], port[MAXLINE];
  char *srcp, filename[MAXLINE], cgiargs[MAXLINE];
  rio_t request_rio, response_rio;

  /* Request 1 - 요청 라인 읽기 [🙋‍♀️ Client -> 🚒 Proxy] */
  Rio_readinitb(&request_rio, clientfd);             // 클라이언트의 요청을 읽기 위해 rio와 fd 연결
  Rio_readlineb(&request_rio, request_buf, MAXLINE); // 요청 라인 읽기
  printf("Request headers:\n %s\n", request_buf);
  sscanf(request_buf, "%s %s", method, uri); // 요청 라인에서 method, uri를 읽어서 지역 변수 `method` `uri`에 할당
  parse_uri(uri, hostname, port, path);
  sprintf(request_buf, "%s %s %s\r\n", method, path, "HTTP/1.0"); // end server에 전송하기 위해 요청 라인 수정
  web_object_t *web_object2 = find_cache(path);
  if (web_object2 != NULL)
  {
    send_cache(web_object2, clientfd);
    read_cache(web_object2);
    return;
  }

  // 요청 메소드가 GET | HEAD가 아닌 경우 예외 처리
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD"))
  {
    clienterror(clientfd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }

  // end server 소켓 생성
  serverfd = is_local_test ? Open_clientfd(hostname, port) : Open_clientfd("52.79.234.188", port);
  if (serverfd < 0)
  {
    clienterror(serverfd, method, "502", "Bad Gateway", "📍 Failed to establish connection with the end server");
    return;
  }

  /* Request 2 - 요청 라인 전송 [🚒 Proxy -> 💻 Server] */
  Rio_writen(serverfd, request_buf, strlen(request_buf));

  /* Request 3 & 4 - 요청 헤더 읽기 & 전송 [🙋‍♀️ Client -> 🚒 Proxy -> 💻 Server] */
  read_requesthdrs(&request_rio, request_buf, serverfd, hostname, port);

  /* Response 1 - 응답 라인 읽기 & 전송 [💻 Server -> 🚒 Proxy -> 🙋‍♀️ Client] */
  Rio_readinitb(&response_rio, serverfd);                   // 서버의 응답을 담을 버퍼 초기화
  Rio_readlineb(&response_rio, response_buf, MAXLINE);      // 응답 라인 읽기
  Rio_writen(clientfd, response_buf, strlen(response_buf)); // 클라이언트에 응답 라인 보내기

  /* Response 2 - 응답 헤더 읽기 & 전송 [💻 Server -> 🚒 Proxy -> 🙋‍♀️ Client] */
  int content_length;
  while (strcmp(response_buf, "\r\n"))
  {
    Rio_readlineb(&response_rio, response_buf, MAXLINE);
    if (strstr(response_buf, "Content-length")) // 응답 바디 수신에 사용하기 위해 바디 사이즈 저장
      content_length = atoi(strchr(response_buf, ':') + 1);
    Rio_writen(clientfd, response_buf, strlen(response_buf));
  }

  /* Response 3 - 응답 바디 읽기 & 전송 [💻 Server -> 🚒 Proxy -> 🙋‍♀️ Client] */
  srcp = malloc(content_length);
  Rio_readnb(&response_rio, srcp, content_length);
  if (content_length <= MAX_OBJECT_SIZE)
  {
    web_object_t *web_object = (web_object_t *)calloc(1, sizeof(web_object_t));
    web_object->response_ptr = srcp;
    web_object->content_length = content_length;
    strcpy(web_object->path, path);
    write_cache(web_object);
    Rio_writen(clientfd, srcp, content_length);
  }
  else
  {
    Rio_writen(clientfd, srcp, content_length);
    free(srcp);
  }
}

// 클라이언트에 에러를 전송하는 함수
// cause: 오류 원인, errnum: 오류 번호, shortmsg: 짧은 오류 메시지, longmsg: 긴 오류 메시지
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* 응답 본문 생성 */
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor="
                "ffffff"
                ">\r\n",
          body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* 응답 출력 */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf)); // 클라이언트에 전송 '버전 에러코드 에러메시지'
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));                              // 컨텐츠 타입
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body)); // \r\n: 헤더와 바디를 나누는 개행
  Rio_writen(fd, buf, strlen(buf));                              // 컨텐츠 크기
  Rio_writen(fd, body, strlen(body));                            // 응답 본문(HTML 형식)
}

// uri를 `hostname`, `port`, `path`로 파싱하는 함수
// uri 형태: `http://hostname:port/path` 혹은 `http://hostname/path` (port는 optional)
void parse_uri(char *uri, char *hostname, char *port, char *path)
{
  // host_name의 시작 위치 포인터: '//'가 있으면 //뒤(ptr+2)부터, 없으면 uri 처음부터
  char *hostname_ptr = strstr(uri, "//") != NULL ? strstr(uri, "//") + 2 : uri;
  char *port_ptr = strchr(hostname_ptr, ':'); // port 시작 위치 (없으면 NULL)
  char *path_ptr = strchr(hostname_ptr, '/'); // path 시작 위치 (없으면 NULL)

  if (port_ptr != NULL) // port 있는 경우
  {
    strncpy(port, port_ptr + 1, path_ptr - port_ptr - 1); // port 구하기
    port[path_ptr - port_ptr - 1] = '\0';
    strncpy(hostname, hostname_ptr, port_ptr - hostname_ptr); // hostname 구하기
  }
  else // port 없는 경우
  {
    if (is_local_test)
      strcpy(port, "80"); // port는 기본 값인 80으로 설정
    else
      strcpy(port, "8000");
    strncpy(hostname, hostname_ptr, path_ptr - hostname_ptr); // hostname 구하기
  }
  strcpy(path, path_ptr); // path 구하기
  return;
}

// Client의 요청을 읽고 Server에 전송하는 함수
// 요청 받은 헤더에 필수 헤더가 없는 경우 추가로 전송
void read_requesthdrs(rio_t *request_rio, void *request_buf, int serverfd, char *hostname, char *port)
{
  int is_host_exist = 0;
  int is_connection_exist = 0;
  int is_proxy_connection_exist = 0;
  int is_user_agent_exist = 0;

  Rio_readlineb(request_rio, request_buf, MAXLINE); // 요청 메시지의 첫번째 줄 읽기

  while (strcmp(request_buf, "\r\n")) // 버퍼에서 읽은 줄이 '\r\n'이 아닐 때까지 반복
  {
    if (strstr(request_buf, "Proxy-Connection") != NULL)
    {
      sprintf(request_buf, "Proxy-Connection: close\r\n");
      is_proxy_connection_exist = 1;
    }
    else if (strstr(request_buf, "Connection") != NULL)
    {
      sprintf(request_buf, "Connection: close\r\n");
      is_connection_exist = 1;
    }
    else if (strstr(request_buf, "User-Agent") != NULL)
    {
      sprintf(request_buf, user_agent_hdr);
      is_user_agent_exist = 1;
    }
    else if (strstr(request_buf, "Host") != NULL)
    {
      is_host_exist = 1;
    }
    Rio_writen(serverfd, request_buf, strlen(request_buf)); // Server에 전송
    Rio_readlineb(request_rio, request_buf, MAXLINE);       // 다음 줄 읽기
  }

  // 필수 헤더 미포함 시 추가로 전송
  if (!is_proxy_connection_exist)
  {
    sprintf(request_buf, "Proxy-Connection: close\r\n");
    Rio_writen(serverfd, request_buf, strlen(request_buf));
  }
  if (!is_connection_exist)
  {
    sprintf(request_buf, "Connection: close\r\n");
    Rio_writen(serverfd, request_buf, strlen(request_buf));
  }
  if (!is_host_exist)
  {
    if (!is_local_test)
      hostname = "52.79.234.188";
    sprintf(request_buf, "Host: %s:%s\r\n", hostname, port);
    Rio_writen(serverfd, request_buf, strlen(request_buf));
  }
  if (!is_user_agent_exist)
  {
    sprintf(request_buf, user_agent_hdr);
    Rio_writen(serverfd, request_buf, strlen(request_buf));
  }

  // 요청 헤더 종료문 전송
  sprintf(request_buf, "\r\n");
  Rio_writen(serverfd, request_buf, strlen(request_buf));
  return;
}

void write_cache(web_object_t *web_object)
{
  if (total_cache_size + web_object->content_length > MAX_CACHE_SIZE)
  {
    // 🚨수정하기
    int temp = lastp->content_length;
    lastp = lastp->left;
    free(lastp->right);
    lastp->right = NULL;
    total_cache_size -= temp;
  }
  if (rootp != NULL)
  {
    web_object->right = rootp;
    rootp->left = web_object;
  }
  rootp = web_object;
  total_cache_size += web_object->content_length;
}

web_object_t *find_cache(char *path)
{
  if (rootp->content_length == 0) // 캐시가 비었으면
    return NULL;
  web_object_t *current = rootp;

  if (strcmp(current->path, path) == 0)
    return current;

  while (strcmp(current->path, path)) // 현재 검사 중인 노드의 path가 다르면 반복
  {
    if (current->right->content_length == 0) // 현재 검사 중인 노드의 오른쪽이 없으면
      return NULL;

    if (strcmp(current->path, path) == 0)
      return current;

    current = current->right;

    if (strcmp(current->path, path) == 0)
      return current;
  }
  return NULL;
}

void send_cache(web_object_t *web_object, int clientfd)
{
  /* 응답 헤더 전송 */
  rio_t rio;
  char buf[MAXLINE];
  sprintf(buf, "HTTP/1.0 200 OK\r\n");                                           // 상태 코드
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);                            // 서버 이름
  sprintf(buf, "%sConnection: close\r\n", buf);                                  // 연결 방식
  sprintf(buf, "%sContent-length: %d\r\n\r\n", buf, web_object->content_length); // 컨텐츠 길이
  Rio_writen(clientfd, buf, strlen(buf));                                        // 클라이언트에 응답 헤더 보내기

  /* 캐싱된 응답 바디 전송 */
  Rio_writen(clientfd, web_object->response_ptr, web_object->content_length); // 클라이언트에 응답 헤더 보내기
}

void read_cache(web_object_t *web_object)
{

  if (web_object == rootp)
    return;

  if (web_object->right->content_length != 0) // 현재 노드의 왼쪽 오른쪽 모두 있는 경우
  {
    // 왼쪽 오른쪽을 이어줌
    web_object_t *right_objtect = web_object->right;
    web_object_t *left_objtect = web_object->left;
    web_object->left->right = right_objtect; // 왼쪽의 오른쪽 노드는 오른쪽이었던 노드가 됨
    web_object->right->left = left_objtect;  // 오른쪽의 왼쪽 노드는 왼쪽이었던 노드가 됨
    web_object->right = rootp;
    rootp = web_object;
  }
  else // 현재 노드의 왼쪽만 있고, 오른쪽이 없는 경우 (현재 노드가 마지막 노드)
  {
    web_object->left->right = NULL;
    web_object->right = rootp;
    rootp = web_object;
  }
}