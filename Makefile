CC = gcc 
CFLAGS = -Wall -g -std=c99

# 定义链接选项变量，默认为空
LDFLAGS =

# 判断是否为 Windows 系统，是的话添加 -lws2_32
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
endif

OBJS = ae.o  anet.o request.o response.o zmalloc.o
SVR_OBJS= svr.o

all : svr

# 链接阶段加入 LDFLAGS，Windows 下自动包含 -lws2_32
svr : $(OBJS) $(SVR_OBJS)
	$(CC) $(CFLAGS) -o svr $^ $(LDFLAGS)

%.o : %.c
	$(CC) $(CFLAGS) -o $@ -c $^

clean :
	rm -rf $(OBJS) $(SVR_OBJS) svr