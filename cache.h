#include <stdio.h>

#include "csapp.h"

typedef struct web_object_t
{
  char path[MAXLINE];
  int content_length;
  char *response_ptr;
  struct web_object_t *prev, *next;
} web_object_t;

web_object_t *find_cache(char *path);
void send_cache(web_object_t *web_object, int clientfd);
void read_cache(web_object_t *web_object);
void write_cache(web_object_t *web_object);

extern web_object_t *rootp;  // 캐시 연결리스트의 root 객체
extern web_object_t *lastp;  // 캐시 연결리스트의 마지막 객체
extern int total_cache_size; // 캐싱된 객체 크기의 총합

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400