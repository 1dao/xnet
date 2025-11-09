// coroutine.h
#ifndef COROUTINE_H
#define COROUTINE_H

#include <vector>
#include <functional>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

// ½Ó¿Úº¯Êý
void		   coroutine_init();
void           coroutine_update();
void           coroutine_finish();
void		   coroutine_add_task(std::function<void()> func);
void           coroutine_wait_read(int fd, long long timeout_ms);
long long      coroutine_current_time();

#endif
