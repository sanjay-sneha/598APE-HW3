FUNC := gcc
copt := -c
OBJ_DIR := ./bin/
FLAGS := -O3 -lm -g -Werror -fopenmp -march=native

C_FILES := $(wildcard src/*.c)
OBJ_FILES := $(addprefix $(OBJ_DIR),$(notdir $(C_FILES:.c=.obj)))

all:
	$(FUNC) ./sandpile.c -o ./sandpile.exe $(FLAGS)

clean:
	rm -f ./*.exe
