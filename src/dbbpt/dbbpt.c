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
#include <string.h>

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

static void split_leaf(
    int fd,
    header_page *header,
    page *leaf,
    int64_t key,
    const char *value,
    int64_t *separator_key,
    int64_t *new_leaf_pgn
);

static void insert_into_parent(
    int fd,
    header_page *header,
    int64_t left_pgn,
    int64_t parent_pgn,
    int64_t separator_key,
    int64_t right_pgn
);
/**
 * @brief Insert a record with the given key and value.
 * @param fd[in] The file descriptor of the database file.
 * @param key[in] The key to insert.
 * @param value[in] The value to insert.
 */
void db_insert(int fd, int64_t key, char *value) {
	// TODO: Implement here

	// 1. header page 읽기
	header_page header;
	load_header_page(fd, &header);

	// 2. B+ tree가 비어있는 경우
	if (header.root_pgn == -1) {
		// 새로운 page 할당
		page *root = alloc_page1(fd, &header);

		// 새 page를 leaf page로 설정
		root->is_leaf = true;
		root->parent_pgn = -1;
		root->num_keys = 1;
		root->right_sibling_pgn = -1;

		// 첫 번째 record 저장
		root->keys[0] = key;

		// 사용자가 입력한 문자열을 leaf page의 record에 복사
		// 복사 최대 크기는 sizeof(root->records[0].value)-1
		strncpy(root->records[0].value, value, sizeof(root->records[0].value) - 1);


		// 마지막 문자열 칸에 문자열 끝남기호 넣음.
		root->records[0].value[
			sizeof(root->records[0].value) - 1
		] = '\0';

		// 새 leaf page를 디스크에 기록
		write_page(fd, root);

		// header의 root page 번호 변경
		header.root_pgn = root->pgn;
		write_header_page(fd, &header);

		// 메모리 해제
		free(root);

		return;
	}

	// 3. root page부터 leaf page까지 내려가기
	page *leaf = malloc(sizeof(page));

	if (leaf == NULL) exit_with_err_msg("Error allocating leaf memory");

	// 루트페이지 가지고 오기
	int64_t cur_pgn = header.root_pgn;
	load_page(fd, cur_pgn, leaf);

	// key에 맞는 leaf page 찾기
	while (!leaf->is_leaf) {
		int i = 0;

		while (i < leaf->num_keys && key >= leaf->keys[i]) { i++; }

		cur_pgn = leaf->child_pgns[i];
		load_page(fd, cur_pgn, leaf);
	}

	// 4. 삽입할 위치 찾기
	int insert_index = 0;

	while (insert_index < leaf->num_keys && leaf->keys[insert_index] < key) {
		insert_index++;
	}

	// 이미 있는 key면 삽입 x
	if (insert_index < leaf->num_keys && leaf->keys[insert_index] == key) {
		free(leaf);
		return;
	}

	// 5. leaf page에 공간이 있는 경우
	if (leaf->num_keys < header.leaf_order && leaf->num_keys < 31) {
		// 삽입 위치 뒤의 key와 record를 오른쪽으로 이동
		for (int j = leaf->num_keys; j > insert_index; j--) {
			leaf->keys[j] = leaf->keys[j-1];
			leaf->records[j] = leaf->records[j-1];
		}

		// 새 key와 value 저장
		leaf->keys[insert_index] = key;

		strncpy(leaf->records[insert_index].value, value,
			sizeof(leaf->records[insert_index].value) - 1);
		leaf->records[insert_index].value[
			sizeof(leaf->records[insert_index].value) - 1
		] = '\0';

		// key 개수 증가
		leaf->num_keys++;

		// 변경된 leaf page를 디스크에 저장
		write_page(fd, leaf);

		free(leaf);
		return;
	}

	// 6. leaf page가 가득 찬 경우
	int64_t separator_key;
	int64_t new_leaf_pgn;

	split_leaf(fd, &header, leaf, key, value, &separator_key, &new_leaf_pgn);

	insert_into_parent(fd, &header, leaf->pgn, leaf->parent_pgn,
						separator_key, new_leaf_pgn);


	free(leaf);
	return;
}

// Helper functions for insertion API
// TODO: define helper functions if you need

// 가득 찬 internal page를 두 개로 분할
static void split_internal(
    int fd,
    header_page *header,
    page *parent,
    int64_t left_pgn,
    int64_t separator_key,
    int64_t right_pgn,
    int64_t *promoted_key,
    int64_t *new_internal_pgn
) {
    // 1. Split된 왼쪽 Child의 위치 찾기
    int left_index = 0;

    while (left_index <= parent->num_keys &&
           parent->child_pgns[left_index] != left_pgn) {
        left_index++;
    }

    if (left_index > parent->num_keys) {
        exit_with_err_msg("Left child not found in internal page");
    }

    // 2. 새 Key와 Child를 포함한 임시 배열
    int old_keys = parent->num_keys;
    int total_keys = old_keys + 1;

	// 임시 저장 배열
    int64_t temp_keys[MAX_INTERNAL_ORDER];
    int64_t temp_children[MAX_INTERNAL_ORDER + 1];

    // 기존 Key 복사 + 새 구분 Key 삽입
    for (int i = 0; i < total_keys; i++) {
        if (i < left_index) {
            temp_keys[i] = parent->keys[i];
        } else if (i == left_index) {
            temp_keys[i] = separator_key;
        } else {
            temp_keys[i] = parent->keys[i - 1];
        }
    }

    // 기존 Child 복사 + 새 오른쪽 Child 삽입
    for (int i = 0; i <= total_keys; i++) {
        if (i <= left_index) {
            temp_children[i] = parent->child_pgns[i];
        } else if (i == left_index + 1) {
            temp_children[i] = right_pgn;
        } else {
            temp_children[i] = parent->child_pgns[i - 1];
        }
    }

    // 3. 가운데 Key를 상위 Parent로 올림
    int split_index = total_keys / 2;

    *promoted_key = temp_keys[split_index];

    int left_count = split_index;
    int right_count = total_keys - split_index - 1; // 하나는 올라갔으므로

    // 4. 새로운 오른쪽 Internal Page 할당
    page *new_internal = alloc_page1(fd, header);

    new_internal->is_leaf = false;
    new_internal->parent_pgn = parent->parent_pgn;

    // 5. 기존 Internal Page를 왼쪽으로 사용
    parent->num_keys = left_count;

    for (int i = 0; i < left_count; i++) {
        parent->keys[i] = temp_keys[i];
    }

    for (int i = 0; i <= left_count; i++) {
        parent->child_pgns[i] = temp_children[i];
    }

    // 6. 오른쪽 Internal Page 구성
    new_internal->num_keys = right_count;

    for (int i = 0; i < right_count; i++) {
        new_internal->keys[i] =
            temp_keys[split_index + 1 + i];
    }

    for (int i = 0; i <= right_count; i++) {
        new_internal->child_pgns[i] =
            temp_children[split_index + 1 + i];
    }

    // 7. 오른쪽 Internal의 자식들에 대해
    //    parent_pgn을 새로운 Internal Page로 변경
    for (int i = 0; i <= right_count; i++) {
        page child;

        load_page(fd, new_internal->child_pgns[i], &child);

        child.parent_pgn = new_internal->pgn;

        write_page(fd, &child);
    }

    // 8. 분할 결과 저장
    *new_internal_pgn = new_internal->pgn;

    write_page(fd, parent);
    write_page(fd, new_internal);

    free(new_internal);
}

// parent에 공간 있을 때 삽입하는 함수
static void insert_into_parent_no_split(int fd, page *parent, int64_t left_pgn,
										int64_t separator_key, int64_t right_pgn
) {
	// 1. 기존 왼쪽 child의 위치 찾기
	int left_index = 0;

	while (left_index <= parent->num_keys 
			&& parent->child_pgns[left_index] != left_pgn) {
		left_index++;
	}

	if (left_index > parent->num_keys) exit_with_err_msg("Left child not found in parent");

	// 2. child 포인터들을 오른쪽으로 한 칸 이동
	for (int i = parent->num_keys + 1; i > left_index + 1; i--) {
		parent->child_pgns[i] = parent->child_pgns[i-1];
	}

	// 3. key들도 오른쪽으로 한 칸 이동
	for (int i = parent->num_keys; i > left_index; i--) {
		parent->keys[i] = parent->keys[i - 1];
	}

	// 4. 새 구분 key와 오른쪽 child 번호 삽입
	parent->keys[left_index] = separator_key;
	parent->child_pgns[left_index + 1] = right_pgn;

	// 5. key 개수 증가
	parent->num_keys++;

	// 6. 변경된 parent를 디스크에 저장
	write_page(fd, parent);
}
// root 만드는 함수
static void create_new_root(int fd, header_page *header, int64_t left_pgn,
							int64_t separator_key, int64_t right_pgn
) {
	// 1. 새로운 root page 할당
	page *root = alloc_page1(fd, header);

	// 2. internal page로 설정
	root->is_leaf = false;
	root->parent_pgn = -1;
	root->num_keys = 1;

	// 3. 구분 key와 자식 page 번호 설정
	root->keys[0] = separator_key;
	root->child_pgns[0] = left_pgn;
	root->child_pgns[1] = right_pgn;

	// 4. 양쪽 자식의 parent_pgn 변경
	page left_child;
	page right_child;

	load_page(fd, left_pgn, &left_child);
	load_page(fd, right_pgn, &right_child);

	left_child.parent_pgn = root->pgn;
	right_child.parent_pgn = root->pgn;

	// 5. 변경된 페이지들을 디스크에 저장
	write_page(fd, &left_child);
	write_page(fd, &right_child);
	write_page(fd, root);

	// 6. header의 root page 번호 변경
	header->root_pgn = root->pgn;
	write_header_page(fd, header);

	// 7. 메모리 해제
	free(root);
}

// leaf 쪼개는 함수
static void split_leaf(
	int fd, header_page *header, page *leaf, int64_t key,
	const char *value, int64_t *separator_key, int64_t *new_leaf_pgn
) {
	int total = leaf->num_keys + 1;

	// 최대 32개의 key + 새 key 1개를 임시 저장
	int64_t temp_keys[MAX_LEAF_ORDER + 1];
	record temp_records[MAX_LEAF_ORDER + 1];

	// 1. 새 key가 들어갈 위치 찾기
	int insert_index = 0;

	while (insert_index < leaf->num_keys && leaf->keys[insert_index] < key) {
		insert_index++;
	}

	// 2. 기존 데이터와 새 데이터를 임시 배열에 정렬하여 저장
	int old_index = 0;

	for (int i = 0; i < total; i++) {
		if (i == insert_index) {
			temp_keys[i] = key;

			strncpy(temp_records[i].value, value, sizeof(temp_records[i].value) - 1);
			temp_records[i].value[
				sizeof(temp_records[i].value) - 1
			] = '\0';
		} else {
			temp_keys[i] = leaf->keys[old_index];
			temp_records[i] = leaf->records[old_index];
			old_index++;
		}
	}
	
	// 3. 새로운 오른쪽 leaf page 할당
	page *new_leaf = alloc_page1(fd, header);

	new_leaf->is_leaf = true;
	new_leaf->parent_pgn = leaf->parent_pgn;

	// 4. key 개수를 나누기
	int left_count = (total + 1) / 2;
	int right_count = total - left_count;

	leaf->num_keys = left_count;
	new_leaf->num_keys = right_count;

	// 5. 왼쪽 leaf에 데이터 복사
	for (int i = 0; i < left_count; i++) {
		leaf->keys[i] = temp_keys[i];
		leaf->records[i] = temp_records[i];
	}

	// 6. 오른쪽 leaf에 데이터 복사
	for (int i = 0; i < right_count; i++) {
		new_leaf->keys[i] = temp_keys[left_count + i];
		new_leaf->records[i] = temp_records[left_count + i];
	}

	// 7. leaf page의 sibling 연결 수정
	new_leaf->right_sibling_pgn = leaf->right_sibling_pgn;
	leaf->right_sibling_pgn = new_leaf->pgn;

	// 8. parent에 올릴 구분 key, 새 페이지 번호 설정
	*separator_key = new_leaf->keys[0];
	*new_leaf_pgn = new_leaf->pgn;

	// 9. 디스크에 저장
	write_page(fd, leaf);
	write_page(fd, new_leaf);

	free(new_leaf);
}

// split을 상위로 전파하는 함수
static void insert_into_parent(
    int fd,
    header_page *header,
    int64_t left_pgn,
    int64_t parent_pgn,
    int64_t separator_key,
    int64_t right_pgn
) {
    // 1. Parent가 없다면 새로운 Root 생성
    if (parent_pgn == -1) {
        create_new_root(
            fd,
            header,
            left_pgn,
            separator_key,
            right_pgn
        );
        return;
    }

    // 2. 기존 Parent Page 읽기
    page parent;
    load_page(fd, parent_pgn, &parent);

    // 3. Parent에 공간이 있는 경우
    if (parent.num_keys < header->internal_order - 1) {
        insert_into_parent_no_split(
            fd,
            &parent,
            left_pgn,
            separator_key,
            right_pgn
        );
        return;
    }

    // 4. Parent도 가득 찬 경우
    int64_t promoted_key;
    int64_t new_internal_pgn;

    split_internal(
        fd,
        header,
        &parent,
        left_pgn,
        separator_key,
        right_pgn,
        &promoted_key,
        &new_internal_pgn
    );

    // 5. 상위 Parent에 구분 Key 삽입
	// 재귀호출
    insert_into_parent(
        fd,
        header,
        parent.pgn,
        parent.parent_pgn,
        promoted_key,
        new_internal_pgn
    );
}

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
