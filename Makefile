TARGET : app

CXXFLAGS=-c -Iinclude -std=c++11 -Isrc
#CXXFLAGS+=-O2
LIBS+=-lcurl -lstdc++

VPATH=src
VPATH+=demo

SRCS=demo_mini_firestore.cpp mini_firestore.cpp
OBJS=$(foreach f,${SRCS},objs/$(basename $f).o)

objs/%.o : %.cpp src/mini_firestore.h Makefile demo/demo_credentials.h
	@echo Compiling $@
	@mkdir -p objs && $(CC) $(CXXFLAGS) $< -o $@

app : ${OBJS}
	@echo Linking $@
	@$(CC) $+ $(LIBS) -o $@

clean :
	rm -f objs/*
	rm -f app
