all: oss user

oss: oss.c
	gcc -lrt -lpthread -o oss oss.c
	
user: user.c
	gcc -lrt -lpthread -o user user.c
	
clean:
	rm oss user *.log