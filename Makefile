## you have command.cc command.h tokenizer.cc tokenizer.h you need compile them and link
## them to one object file called  : "myshell" 
## so we should run make , that will create our object myshell ,
## ./myshell should run your application 
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic

all: myshell

myshell: command.o tokenizer.o
	$(CXX) $(CXXFLAGS) command.o tokenizer.o -o myshell

command.o: command.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c command.cc -o command.o

tokenizer.o: tokenizer.cc tokenizer.h
	$(CXX) $(CXXFLAGS) -c tokenizer.cc -o tokenizer.o

clean:
	rm -f command.o tokenizer.o myshell myshell.log

.PHONY: all clean
