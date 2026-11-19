#include "file_manager.h"
#include "dbbpt.h"

#include <stdbool.h>
#ifdef _WIN32
#define bool char
#define false 0
#define true 1
#endif
#include <stdio.h>
#include <stdlib.h>

// FUNCTION DEFINITIONS.
// Find API
/**
 * @brief Find a record with the given key.
 * @param fd[in] The file descriptor of the database file.
 * @param key[in] The key to find.
 * @param verbose[in] Whether to print verbose output.
 * @param leaf_out[out] The leaf page pointer where the key is found.
 * @return The record with the given key, or NULL if not found.
 */
record *db_find(int fd, int64_t key, bool verbose, page **leaf_out) {
	// TODO: Implement here
	
	// 1. 호출자에게 전달할 leaf 포인터 초기화
	if (leaf_out != NULL) *leaf_out = NULL;

	// 2. header page에서 root page 번호 읽기
	header_page header;
	load_header_page(fd, &header);

	// 트리가 비어있으면 검색 실패
	if (header.root_pgn == -1) return NULL;

	// 3. 페이지를 읽기 위한 메모리 공간 확보
	page *cur = malloc(sizeof(page));

	if (cur == NULL) exit_with_err_msg("Error allocating page memory");

	int64_t cur_pgn = header.root_pgn;
	load_page(fd, cur_pgn, cur);

	// 4. leaf page에 도착할 때까지 탐색
	while (!cur->is_leaf) {
		if (verbose) printf("Page %lld\n", (long long)cur_pgn);

		int i = 0;

		while (i < cur->num_keys && key >= cur->keys[i]) { i++; } // 원하는 자식노드로 감.

		cur_pgn = cur->child_pgns[i];
		load_page(fd, cur_pgn, cur);
	}

	// 5. leaf page에서 key 검색
	for (int i = 0; i < cur->num_keys; i++) {
		if (cur->keys[i] == key) {
			// result는 cur속 record 속성을 가리키는 포인터.
			// 따라서 cur이 없어지면 result도 위험.
			record *result = &cur->records[i];

			if (leaf_out != NULL) { // leaf_out이 존재 할 때
				*leaf_out = cur;
			} else { // leaf_out이 없을 때
				// 소유할 leaf 없으니 새로 하나 만들어줘서 거기에 넣음
				record *copy = malloc(sizeof(record));

				if (copy == NULL) {
					free(cur);
					exit_with_err_msg("Error allocating record memory");
				}

				*copy = *result;
				free(cur);
				return copy;
			}

			return result;
		}
	}

	// 6. key가 없으면 메모리 해제 후 null 반환
	free(cur);
	return NULL;
}

// Helper functions for find API
// TODO: define helper functions if you need

// Insertion API
/**
 * @brief Insert a record with the given key and value.
 * @param fd[in] The file descriptor of the database file.
 * @param key[in] The key to insert.
 * @param value[in] The value to insert.
 */
void db_insert(int fd, int64_t key, char *value) {
	// TODO: Implement here
}

// Helper functions for insertion API
// TODO: define helper functions if you need

// Deletion API
/**
 * @brief Delete a record with the given key.
 * @param fd[in] The file descriptor of the database file.
 * @param key[in] The key to delete.
 */
void db_delete(int fd, int64_t key) {
	// TODO: Implement here
}

// Helper functions for delete API
// TODO: define helper functions if you need

// Destroy API
void db_destroy(int fd) {
	header_page header;
	load_header_page(fd, &header);
	destroy_pages(fd, header.root_pgn);
	header.root_pgn = -1;
	write_header_page(fd, &header);
}

// Helper functions for destroy API
void destroy_pages(int fd, int64_t pgn) {
	if (pgn <= 0) return;
	page cur_page;
	load_page(fd, pgn, &cur_page);
	if (!cur_page.is_leaf) {
		for (int i = 0; i < cur_page.num_keys + 1; i++) destroy_pages(fd, cur_page.child_pgns[i]);
	}
	free_page(fd, pgn);
}

// Utility helper functions
// TODO: define helper functions if you need
