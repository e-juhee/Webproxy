#include <stdio.h>

#include "csapp.h"

void doit(int clientfd, char *hostname, char *port);
void read_requesthdrs(rio_t *rp, void *buf);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv)
{
    int listenfd, clientfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

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
        clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);                     // 클라이언트 연결 요청 수신
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // 클라이언트의 호스트 이름과 포트 번호 파악

        printf("PROXY:: Accepted connection from (%s, %s)\n", hostname, port);

        // 클라이언트의 요청을 받아서 엔드 서버로 전송 & 엔드 서버의 응답을 받아서 클라이언트로 전송
        doit(clientfd, hostname, port);

        Close(clientfd); // 연결 종료
    }
}

void doit(int clientfd, char *hostname, char *port)
{
    int serverfd;
    int is_static;
    struct stat sbuf;
    char request_buf[MAXLINE], response_buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // MAXLINE: 8192
    char *srcp, *lengthp, filename[MAXLINE], cgiargs[MAXLINE];
    rio_t request_rio, response_rio; // 버퍼

    // end server socket 생성
    if ((serverfd = Open_clientfd("ec2-52-79-234-188.ap-northeast-2.compute.amazonaws.com", "8000")) < 0)
    {
        return;
    }

    /* Request 1 - 요청 라인 읽기 */
    Rio_readinitb(&request_rio, clientfd);                  // 클라이언트의 요청을 담을 버퍼 초기화
    Rio_readlineb(&request_rio, request_buf, MAXLINE);      // 요청 라인 읽기
    Rio_writen(serverfd, request_buf, strlen(request_buf)); // 서버에게 요청 라인 보내기

    printf("Request headers:\n");
    printf("%s", request_buf);
    sscanf(request_buf, "%s %s %s", method, uri, version); // buf 문자열에서 method, uri, version을 읽어온다.

    // 요청 메소드가 GET||HEAD가 아니면 에러 처리
    if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD"))
    {
        clienterror(clientfd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }

    /* Request 2 - 요청 헤더 읽기 */
    Rio_readlineb(&request_rio, request_buf, MAXLINE);      // 요청 라인 읽기
    Rio_writen(serverfd, request_buf, strlen(request_buf)); // 서버에게 요청 라인 보내기
    while (strcmp(request_buf, "\r\n"))                     // '\r\n'이 아닐 때까지 반복
    {
        Rio_readlineb(&request_rio, request_buf, MAXLINE);
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }

    /* Response 1 - 응답 라인 읽기 */
    Rio_readinitb(&response_rio, serverfd);                   // 서버의 응답을 담을 버퍼 초기화
    Rio_readlineb(&response_rio, response_buf, MAXLINE);      // 응답 라인 읽기
    Rio_writen(clientfd, response_buf, strlen(response_buf)); // 클라이언트에 응답 라인 보내기

    /* Response 2 - 응답 헤더 읽기 */
    int content_length;
    while (strcmp(response_buf, "\r\n")) // 버퍼에서 읽은 줄이 '\r\n'이 아닐 때까지 반복 (strcmp: 두 인자가 같으면 0 반환)
    {
        Rio_readlineb(&response_rio, response_buf, MAXLINE);
        if (strstr(response_buf, "Content-length") != 0)
        {
            lengthp = strchr(response_buf, ':');
            content_length = atoi(lengthp + 1);
        }
        Rio_writen(clientfd, response_buf, strlen(response_buf)); // 클라이언트에 응답 라인 보내기
    }

    /* Response 3 - 응답 바디 읽기 */
    if (content_length)
    {
        srcp = malloc(content_length);
        Rio_readnb(&response_rio, srcp, content_length);
        Rio_writen(clientfd, srcp, content_length);
        free(srcp);
    }

    // response body size 구하기

    // Content-length 헤더의 위치 찾기
    // char *content_length_ptr = ;
    // if (content_length_ptr == NULL) {
    //     // Content-length 헤더를 찾을 수 없는 경우
    //     content_length = 0;
    // }
    // else
    // {
    //     // Content-length 값을 추출
    //     if (sscanf(content_length_ptr + strlen("Content-length: "), "%d", &content_length) != 1) {
    //         // Content-length 값을 읽을 수 없는 경우
    //         content_length = 0;
    //     }
    // }
}

// 클라이언트에 에러를 전송하는 함수(cause: 오류 원인, errnum: 오류 번호, shortmsg: 짧은 오류 메시지, longmsg: 긴 오류 메시지)
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF]; // buf: HTTP 응답 헤더, body: HTML 응답의 본문인 문자열(오류 메시지와 함께 HTML 형태로 클라이언트에게 보여짐)

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

void read_requesthdrs(rio_t *rp, void *buf)
{
    // 🚨 헤더 검증 추가하기

    Rio_readlineb(rp, buf, MAXLINE); // 요청 메시지의 첫번째 줄 읽기

    while (strcmp(buf, "\r\n")) // 버퍼에서 읽은 줄이 '\r\n'이 아닐 때까지 반복 (strcmp: 두 인자가 같으면 0 반환)
    {
        Rio_readlineb(rp, buf, MAXLINE);
    }
    // sprintf(buf, "Connection: close");
    // sprintf(buf, "Proxy-Connection: close");
    // sprintf(buf, user_agent_hdr);
    return;
}