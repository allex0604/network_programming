CC = g++

all : np_single_proc

np_simple:
	$(CC) np_simple.cpp -g -o np_simple
np_single_proc:
	$(CC) np_single_proc.cpp -g -o np_single_proc
np_multi_proc:
	$(CC) np_multi_proc.cpp -g -o np_multi_proc -lrt

clean :
	rm -rf np_simple np_single_proc np_multi_proc