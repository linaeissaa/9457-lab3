## you have command.cc command.h tokenizer.cc tokenizer.h you need compile them and link
## them to one object file called  : "myshell" 
## so we should run make , that will create our object myshell ,
## ./myshell should run your application 
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)

command.o: command.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c command.cc -o command.o

tokenizer.o: tokenizer.cc tokenizer.h
	$(CXX) $(CXXFLAGS) -c tokenizer.cc -o tokenizer.o

clean:
	rm -f $(OBJECTS) $(TARGET) myshell.log

.PHONY: all clean
