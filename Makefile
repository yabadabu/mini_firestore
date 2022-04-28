TARGET : app

CXXFLAGS=-c -Iinclude -std=c++11 -Isrc
#CXXFLAGS+=-O2
LIBS+=-lcurl -lstdc++

VPATH=src
VPATH+=demo

OBJS_PATH=objs
SRCS=demo_mini_firestore.cpp mini_firestore.cpp
OBJS=$(foreach f,${SRCS},$(OBJS_PATH)/$(basename $f).o)

$(OBJS_PATH)/%.o : %.cpp src/mini_firestore.h Makefile demo/demo_credentials.h | $(OBJS_PATH)
	@echo Compiling $@
	@$(CC) $(CXXFLAGS) $< -o $@

app : ${OBJS}
	@echo Linking $@
	@$(CC) $+ $(LIBS) -o $@

$(OBJS_PATH) :
	@echo Creating temporal folder
	@mkdir $(OBJS_PATH)

clean :
	rm -f objs/*
	rm -f app
