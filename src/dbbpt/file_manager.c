#include "file_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>


/**
 * @brief Open a database file or create a new one if it doesn't exist.
 * @param file_path[in] The path to the database file.
 * @param leaf_order[in] The leaf order of the B+ tree.
 * @param internal_order[in] The internal order of the B+ tree.
 * @return The file descriptor of the database file. Return -1 if failed.
 *
 * Set the header page and free pages pool during tree creation. Plus, use -1
 * to represent a NULL page number for fields such as the initial root page number
 * in header page or next page number of the last free page.
 */
int open_or_create_tree(const char *file_path, int leaf_order, int internal_order) {
	// TODO: Implement here
	// 1. 기존 파일 열기 (O_RDWR는 읽기쓰기 다 되게 FILE 연다는 뜻)
	int fd = open(file_path, O_RDWR);

	if (fd >= 0) {
		return fd;
	}

	// file이 없는 경우에만 새로 생성
	// ENOENT는 파일이 존재하지 않는다는 오류 코드
	// 즉, 파일이 없는 것이 아니라 권한 등의 문제일 경우 새 파일 안 만들기
	if (errno != ENOENT) {
		return -1;
	}

	// 2. 새 DB 파일 생성
	// O_EXCL은 O_CREAT와 함께 사용하여 파일이 이미 존재한다면 생성에 실패.
	fd = open(file_path, O_RDWR | O_CREAT | O_EXCL, 0644);

	if (fd < 0) {
		return -1;
	}

	// 3. 초기 파일 크기 확보
	// ftruncate는 파일의 크기를 지정한 크기로 변경하는 함수. 변경 실패시 -1 반환
	if (ftruncate(fd, (off_t)INIT_PAGE_COUNT * PAGE_SIZE) == -1) {
		close(fd);
		return -1;
	}

	// 4. Header Page 초기화
	header_page header = {0}; // 모든 필드의 값을 0으로 맞춤.

	header.free_pgn = 1;
	header.root_pgn = -1;
	header.num_pages = INIT_PAGE_COUNT;
	header.leaf_order = leaf_order;
	header.internal_order = internal_order;

	// 5. Free Page들을 연결한다.
	for (int64_t i = 1; i< INIT_PAGE_COUNT; i++) {
		int64_t next_pgn = (i == INIT_PAGE_COUNT -1) ? -1 : i+1;
		off_t offset = (off_t)i * PAGE_SIZE;

		// pwrite는 실제로 기록한 바이트 수를 반환함. 
		if (pwrite(fd, &next_pgn, sizeof(next_pgn), offset) != sizeof(next_pgn)) {
			close(fd);
			return -1;
		}
	}

	// 6. Header Page를 디스크에 기록
	write_header_page(fd, &header);

	return fd;
}

/**
 * @brief Load the header page from the database file.
 * @param fd[in] The file descriptor of the database file.
 * @param dest[out] The destination to store the header page.
 */
void load_header_page(int fd, header_page* dest) {
	off_t cur_offset = 0;
	if (pread(fd, &(dest->free_pgn), 8, cur_offset) == -1) exit_with_err_msg("Error on loading free page number at header page.");
	cur_offset += 8;

	if (pread(fd, &(dest->root_pgn), 8, cur_offset) == -1) exit_with_err_msg("Error on loading root page number at header page.");
	cur_offset += 8;

	if (pread(fd, &(dest->num_pages), 8, cur_offset) == -1) exit_with_err_msg("Error on loading number of pages at header page.");
	cur_offset += 8;

	if (pread(fd, &(dest->leaf_order), 4, cur_offset) == -1) exit_with_err_msg("Error on loading leaf order at header page.");
	cur_offset += 4;

	if (pread(fd, &(dest->internal_order), 4, cur_offset) == -1) exit_with_err_msg("Error on loading internal order at header page.");
	cur_offset += 4;
}

/**
 * @brief Write the header page to the database file.
 * @param fd[in] The file descriptor of the database file.
 * @param src[in] The header page to write.
 */
void write_header_page(int fd, const header_page* src) {
	off_t cur_offset = 0;
	if (pwrite(fd, &(src->free_pgn), 8, cur_offset) < 8) exit_with_err_msg("Error on writing free page number at header page.");
	cur_offset += 8;

	if (pwrite(fd, &(src->root_pgn), 8, cur_offset) < 8) exit_with_err_msg("Error on writing root page number at header page.");
	cur_offset += 8;

	if (pwrite(fd, &(src->num_pages), 8, cur_offset) < 8) exit_with_err_msg("Error on writing number of pages at header page.");
	cur_offset += 8;

	if (pwrite(fd, &(src->leaf_order), 4, cur_offset) < 4) exit_with_err_msg("Error on writing leaf order at header page.");
	cur_offset += 4;

	if (pwrite(fd, &(src->internal_order), 4, cur_offset) < 4) exit_with_err_msg("Error on writing internal order at header page.");
	cur_offset += 4;
}

/**
 * @brief Load a page from the database file.
 * @param fd[in] The file descriptor of the database file.
 * @param pgn[in] The page number of the page to load.
 * @param dest[out] The destination to store the page.
 */
void load_page(int fd, int64_t pgn, page* dest) {
	// TODO: Implement here
}

/**
 * @brief Write a page to the database file.
 * @param fd[in] The file descriptor of the database file.
 * @param src[in] The page to write. Return NULL if failed.
 */
void write_page(int fd, const page* src) {
	// TODO: Implement here
}

/**
 * @brief Allocate a new page.
 * @param fd[in] The file descriptor of the database file.
 * @return The new page.
 *
 * If no free pages are left, this function doubles the free pages pool, and then allocates one.
 * If memory allocation fails, then kill the process using the `exit_with_err_msg()` function.
 */
page *alloc_page(int fd) {
	header_page header;
	load_header_page(fd, &header);

	return alloc_page1(fd, &header);
}

/**
 * @brief Allocate a new page. This function uses the passed header_page pointer instead of calling load_header_page.
 * @param fd[in] The file descriptor of the database file.
 * @param header_page[in] The header page.
 * @return The new page.
 *
 * If no free pages are left, this function doubles the free pages pool, and then allocates one.
 * If memory allocation fails, then kill the process using the `exit_with_err_msg()` function.
 */
page *alloc_page1(int fd, header_page *header_page) {
	// TODO: Implement here
	return NULL;
}

/**
 * @brief Free a page.
 * @param fd[in] The file descriptor of the database file.
 * @param pgn[in] The page number of the page to free.
 */
void free_page(int fd, int64_t pgn) {
	// TODO: Implement here
}

/**
 * @brief Print an error message and exit the program.
 * @param err_msg[in] The error message to print.
 */
void exit_with_err_msg(const char* err_msg) {
	perror(err_msg);
	exit(EXIT_FAILURE);
}
