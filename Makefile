# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executable 
TARGET = Pacmanist

# Objects variables
OBJS = game.o display.o board.o parser.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Subproject directories
CLIENT_SUBDIR = client-base-with-Makefile-v3

# Make targets
all: server client

server: $(BIN_DIR)/$(TARGET)

client:
	$(MAKE) -C $(CLIENT_SUBDIR) all

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(SLEEP) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# run the program
run: pacmanist
	@./$(BIN_DIR)/$(TARGET) $(ARGS)  # to run use: make run ARGS="<folder>"

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)
	$(MAKE) -C $(CLIENT_SUBDIR) clean

# indentify targets that do not create files
.PHONY: all clean run folders server client
