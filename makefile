all: oss worker

oss: oss.cpp
        g++ -o oss oss.cpp

worker: worker.cpp
        g++ -o worker worker.cpp

clean:
        rm -f oss worker
