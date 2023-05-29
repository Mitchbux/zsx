#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#include <compressapi.h>

#include "sfx.c"

#define sx_chunk_bits 22
#define sx_chunk (1<<sx_chunk_bits)

typedef uint8_t byte;
size_t bytes_read;

void CALLBACK FIO(DWORD E1, DWORD N2, LPOVERLAPPED ol) {
	bytes_read = N2;
}

int main(int argc, char **argv) {
	if(argc < 2)
	{
		puts("usage : zsx.exe filename");
		return 0;
	}
	char *filename = argv[1];
	OVERLAPPED ol;
	ol.Offset  = 0;
	ol.OffsetHigh = 0;
	
	COMPRESSOR_HANDLE Compressor = NULL;
	CreateCompressor(2, NULL, &Compressor);

	HANDLE hFile = CreateFile(
		filename,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		printf("Can't read file!\n");
		return 0;
	};

	int encoded = 0;
	DWORD file_high;
	int file_size = GetFileSize(hFile, &file_high);

	byte *in = (byte *)malloc(sx_chunk);
	
	
	char outfile[256];
	strcpy(outfile, filename);
	strcat(outfile, ".exe");
	HANDLE hOutFile = CreateFile(
			outfile,
			GENERIC_WRITE,
			0,
			0,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			0);

	if (hOutFile == INVALID_HANDLE_VALUE) {
		printf("Can't create file!\n");
		return 0;
	};

	char txtfile[256];
	strcpy(txtfile, filename);
	strcat(txtfile, ".txt");

	printf("encoding...\n");
	while (encoded < file_size) 
	{
		
		ReadFileEx(hFile, in, sx_chunk, &ol, FIO);
		SleepEx(3000, TRUE);
		encoded += bytes_read;
		ol.Offset += bytes_read;
		
		size_t csize;
		Compress(Compressor, in, bytes_read, NULL, 0, &csize);
		byte *out = (byte *)malloc(csize);
		Compress(Compressor, in, bytes_read, out, csize, &csize);
		DWORD asize;
		int page_size = (((csize+0x100)/0x200)+1)*0x200;
		int global_size = (((page_size+0x1000)/0x1000)+4)*0x1000;
		printf("%d ", page_size);
		memset(in, 0, bytes_read);
		memcpy(in, sfx, 0x1000);
		*(int*)(in+0xD0)=global_size;
		*(int*)(in+0x1F8)=csize+0x0100;
		*(int*)(in+0x200)=page_size;
		*(int*)(in+0xEFC)=csize;
		*(int*)(in+0xEF8)=bytes_read;
		memcpy(in+0xE28, txtfile, strlen(txtfile)+1);
		memcpy(in+0xF00, out, csize);
		WriteFile(hOutFile, in, 0xE00+page_size, &asize, NULL);
		free(out);
		printf("%.2f%%\n", (double)encoded / (double)file_size * 100.0);
	}
	CloseHandle(hFile);
	CloseHandle(hOutFile);
	CloseCompressor(Compressor);
	free(in);
	char msg[256];
	strcpy(msg,"File write ok : ");
	strcat(msg, outfile);
	puts(msg);
	return 0;
}
