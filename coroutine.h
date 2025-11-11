// coroutine.h
#ifndef COROUTINE_H
#define COROUTINE_H

#ifdef __str
#undef __str
#endif
#include <vector>
#include <functional>


// ½Ó¿Úº¯Êý
void		   coroutine_init();
void           coroutine_update();
void           coroutine_finish();
void		   coroutine_add_task(std::function<void()> func);
void           coroutine_wait_read(int fd, long long timeout_ms);
long long      coroutine_current_time();

#endif
