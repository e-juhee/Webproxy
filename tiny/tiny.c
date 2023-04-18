/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include <signal.h>
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
    int listenfd, connfd;
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
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);                       // line:netp:tiny:accept 클라이언트 연결 요청 수신
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // 클라이언트의 호스트 이름과 포트 번호 파악
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);  // line:netp:tiny:doit 클라이언트의 요청 처리
        Close(connfd); // line:netp:tiny:close 연결 종료
    }
}

void doit(int fd)
{
    signal(SIGPIPE, SIG_IGN);

    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // MAXLINE: 8192
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio; // 버퍼

    /* 요청 라인과 헤더 읽기 */
    Rio_readinitb(&rio, fd);           // 버퍼(rio)를 초기화하고, rio와 파일 디스크립터(fd)를 연결한다.
    Rio_readlineb(&rio, buf, MAXLINE); // 버퍼(rio)에서 한줄씩 읽어서 buf에 저장한다. MAXLINE보다 길면 자동으로 버퍼를 증가시켜서 /r과 /n 문자를 모두 읽을 때까지 계속 읽어들인다.
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version); // buf 문자열에서 method, uri, version을 읽어온다.
    if (strcasecmp(method, "GET"))
    { // 요청 메소드가 GET이 아니면 에러 처리
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }
    read_requesthdrs(&rio); // 요청 헤더를 읽고 파싱한다.(요청 헤더를 읽어서 클라이언트가 요청한 추가 정보를 처리한다.)

    /* URI 파싱 */
    is_static = parse_uri(uri, filename, cgiargs); // 요청이 정적 콘텐츠인지 동적 콘텐츠인지 파악한다.
    if (stat(filename, &sbuf) < 0)
    { // 파일이 디스크에 없으면 에러 처리
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static)
    { // 정적 컨텐츠인 경우
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
        { // 일반 파일이 아니거나 읽기 권한이 없는 경우 에러 처리
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size, method); // 정적 컨텐츠 제공
    }
    else
    { // 동적 컨텐츠인 경우
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
        { // 일반 파일이 아니거나 실행 권한이 없는 경우 에러 처리
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs, method); // 동적 컨텐츠 제공
    }
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

void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE); // 요청 메시지의 첫번째 줄 읽기
    printf("%s", buf);               // 헤더 필드 출력
    while (strcmp(buf, "\r\n"))      // 버퍼에서 읽은 줄이 '\r\n'이 아닐 때까지 반복 (strcmp: 두 인자가 같으면 0 반환)
    {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf); // 헤더 필드 출력
    }
    return;
}

// URI에서 요청된 파일 이름과 CGI 인자 추출
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (!strstr(uri, "cgi-bin"))
    { // uri에 cgi-bin이 없으면 정적 컨텐츠
        strcpy(cgiargs, "");
        strcpy(filename, ".");             // filename에 현재 디렉터리를 넣고
        strcat(filename, uri);             // filename에 uri를 이어 붙인다.
        if (uri[strlen(uri) - 1] == '/')   // uri가 /로 끝나면
            strcat(filename, "home.html"); //  filename에 home.html를 이어 붙인다.
        return 1;                          // 정적 컨텐츠일 때는 1 리턴
    }
    else
    { // 동적 컨텐츠
        ptr = index(uri, '?');
        if (ptr)
        {
            strcpy(cgiargs, ptr + 1);
            *ptr = '\0';
        }
        else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}

void serve_static(int fd, char *filename, int filesize, char *method)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];
    rio_t rio;
    /* 응답 헤더 전송 */
    get_filetype(filename, filetype);                          // 파일 타입 결정
    sprintf(buf, "HTTP/1.0 200 OK\r\n");                       // 상태 코드
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);        // 서버 이름
    sprintf(buf, "%sConnection: close\r\n", buf);              // 연결 방식
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);   // 컨텐츠 길이
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype); // 컨텐츠 타입
    Rio_writen(fd, buf, strlen(buf));                          // buf에서 fd로 전송(헤더 정보 전송)
    printf("Response headers:\n");
    printf("%s", buf);

    /* HTTP HEAD 메소드 처리 */
    if (strcasecmp(method, "HEAD") == 0)
    {
        return; // 응답 바디를 전송하지 않음
    }

    /* 응답 바디 전송 */
    srcfd = Open(filename, O_RDONLY, 0); // 파일 열기
    // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); // 파일을 가상메모리에 매핑
    srcp = malloc(filesize);          // 파일을 위한 메모리 할당
    Rio_readinitb(&rio, srcfd);       // 파일을 버퍼로 읽기위한 초기화
    Rio_readn(srcfd, srcp, filesize); // 읽기
    Close(srcfd);                     // 파일 디스크립터 닫기
    Rio_writen(fd, srcp, filesize);   // 파일 내용을 클라이언트에게 전송 (응답 바디 전송)
    free(srcp);                       // 매핑된 가상메모리 해제
}

void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpg");
    else if (strstr(filename, ".mp4"))
        strcpy(filetype, "video/mp4");
    else
        strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs, char *method)
{
    char buf[MAXLINE], *emptylist[] = {NULL};

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) // 자식 프로세스 포크
    {
        setenv("QUERY_STRING", cgiargs, 1);   // QUERY_STRING 환경 변수를 URI에서 추출한 CGI 인수로 설정
        setenv("REQUEST_METHOD", method, 1);  // QUERY_STRING 환경 변수를 URI에서 추출한 CGI 인수로 설정
        Dup2(fd, STDOUT_FILENO);              // 자식 프로세스의 표준 출력을 클라이언트 소켓에 연결된 파일 디스크립터로 변경
        Execve(filename, emptylist, environ); // 현재 프로세스의 이미지를 filename 프로그램으로 대체
    }
    Wait(NULL);
}