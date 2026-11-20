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

static void repair_internal(
    int fd,
    header_page *header,
    int64_t node_pgn
);

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

	// 1. header page 읽기
	header_page header;
	load_header_page(fd, &header);

	// 빈 트리라면 삭제x
	if (header.root_pgn == -1) return;

	// 2. 삭제할 key 있는 leaf 찾기
	page leaf;
	int64_t cur_pgn = header.root_pgn;

	load_page(fd, cur_pgn, &leaf);
	while (!leaf.is_leaf) {
		int i = 0;
		while (i < leaf.num_keys && key >= leaf.keys[i]) { i++; }

		cur_pgn = leaf.child_pgns[i];
		load_page(fd, cur_pgn, &leaf);
	}

	// 3. leaf page에서 삭제할 key 위치 찾기
	int delete_index = 0;
	while (delete_index < leaf.num_keys && leaf.keys[delete_index] != key) {
		delete_index++;
	}

	// key 못 찾으면 종료.
	if (delete_index == leaf.num_keys) return;

	// 4. key, record 한 칸 왼쪽으로
	for (int i = delete_index; i < leaf.num_keys - 1; i++) {
		leaf.keys[i] = leaf.keys[i + 1];
		leaf.records[i] = leaf.records[i + 1];
	}

	leaf.num_keys--;

	// 5. root가 leaf인 경우
	if (leaf.pgn == header.root_pgn) {
		if (leaf.num_keys == 0) {
			// 트리가 완전히 빔.
			header.root_pgn = -1;
			write_header_page(fd, &header);
			free_page(fd, leaf.pgn);
		} else {
			write_page(fd, &leaf);
		}

		return;
	}

	// 6. 일반 leaf인 경우
	write_page(fd, &leaf);

	// 7. 최소 key 개수 계산
	int min_keys = (header.leaf_order + 1) / 2;

	// 최소 개수 만족하면 재분배 필요 없음.
	if (leaf.num_keys >= min_keys) {
		refresh_tree_after_delete(fd);
		return;
	}

	// 8. parent page 읽기
	page parent;
	load_page(fd, leaf.parent_pgn, &parent);

	// parent에서 현재 leaf의 위치 찾기
	int leaf_index = 0;
	while (leaf_index <= parent.num_keys && 
			parent.child_pgns[leaf_index] != leaf.pgn) {
		leaf_index++;
	}

	if (leaf_index > parent.num_keys) exit_with_err_msg("Leaf not found in parent");

	// 9. 왼쪽 형제에게 빌릴 수 있는지 확인
	if (leaf_index > 0) {
		page left_sibling;

		load_page(fd, parent.child_pgns[leaf_index - 1], &left_sibling);

		if (left_sibling.num_keys > min_keys) {
			redistribute_leaf(fd, &leaf, &left_sibling, &parent, leaf_index, true);
			refresh_tree_after_delete(fd);
			return;
		}
	}

	// 10. 오른쪽 형제에게 빌릴 수 있는지 확인
	if (leaf_index < parent.num_keys) {
		page right_sibling;
		
		load_page(fd, parent.child_pgns[leaf_index + 1], &right_sibling);

		if (right_sibling.num_keys > min_keys) {
			redistribute_leaf(fd, &leaf, &right_sibling, &parent, leaf_index, false);
			refresh_tree_after_delete(fd);
			return;
		}
	}

	// 11. 어느 형제에게서도 빌릴 수 없는 경우
	// 왼쪽 형제가 있으면 왼쪽 leaf에 병합
	if (leaf_index > 0) {
		page left_sibilng;
		load_page(fd, parent.child_pgns[leaf_index - 1], &left_sibling);

		coalesce_leaf(fd, &left_sibling, &leaf);

		// 왼쪽 형제와 현재 leaf 사이의 구분 key 제거
		remove_from_internal(fd, &parent, leaf_index - 1);
	} else {
		// 왼쪽 형제가 없으면 오른쪽 leaf를 현재 leaf에 병합
		page right_sibling;
		load_page(fd, parent.child_pgns[leaf_index + 1], &right_sibling);

		coalesce_leaf(fd, &leaf, &right_sibling);

		// 현재 leaf와 오른쪽 형제 사이의 구분 key 제거
		remove_from_internal(fd, &parent, leaf_index);
	}

	// 12. leaf 병합으로 key가 감소한 parent 복구
	repair_internal(fd, &header, parent.pgn);
	return;
}

// Helper functions for delete API
// TODO: define helper functions if you need

// 병합을 위한 함수
static void coalesce_leaf(int fd, page *left, page *right) {
	// 1. 오른쪽 leaf의 모든 record를 왼쪽 leaf 끝에 복사
	int start = left->num_keys;

	for (int i = 0; i < right->num_keys; i++) {
		left->keys[start + i] = right->keys[i];
		left->records[start + i] = right->records[i];
	}

	// 2. 왼쪽 leaf의 key 개수 증가
	left->num_keys += right->num_keys;

	// 3. leaf간 연결 수정
	left->right_sibling_pgn = right->right_sibling_pgn;

	// 4. 병합 결과를 디스크에 기록
	write_page(fd, left);

	// 5. 안 쓰는 오른쪽 페이지 반환
	free_page(fd, right->pgn);
}

// 병합 시 부모 페이지 key 설정 함수
static void remove_from_internal(int fd, page *parent, int key_index) {
	// 1. 삭제할 key 뒤의 key들을 왼쪽으로 이동
	for (int i = key_index; i < parent->num_keys - 1; i++) {
		parent->keys[i] = parent->keys[i + 1];
	}

	// 2. 삭제할 key의 오른쪽 child 포인터 제거
	for (int i = key_index + 1; i < parent->num_keys; i++) {
		parent->child_pgns[i] = parent->child_pgns[i + 1];
	}

	// 3. key 개수 감소
	parent->num_keys--;

	// 4. 변경된 parent 저장
	write_page(fd, parent);
}

// 병합 이후 parent가 root가 되고 key가 0개일 경우 기존 루트 제거, 남은 child를 root로 지정
static void adjust_root_after_delete(int fd, header_page *header, page *root) {
	// root에 key가 남아있다면 변경할 필요 없음
	if (root->num_keys > 0) return;

	// internal root의 유일한 child를 새로운 root로 지정
	int64_t new_root_pgn = root->child_pgns[0];

	page new_root;
	load_page(fd, new_root_pgn, &new_root);

	new_root.parent_pgn = -1;
	write_page(fd, &new_root);

	// header 변경
	header->root_pgn = new_root_pgn;
	write_header_page(fd, header);

	// 기존 root 반환
	free_page(fd, root->pgn);
}

// 재분배 시 왼쪽 형제와 오른쪽 형제 양쪽에서 빌려오는 경우 처리.
// 왼쪽할지 오른쪽 할지는 미리 정해져서 들어옴.from_leaf로 처리.
static void redistribute_leaf(int fd, page *leaf, page *sibling, page *parent,
								int leaf_index, bool from_left) {
	if (from_left) { // 왼쪽에서 가져오기
		// 1. leaf의 기존 데이터들 오른쪽으로 이동
		for (int i = leaf->num_keys; i > 0; i--) {
			leaf->keys[i] = leaf->keys[i - 1];
			leaf->records[i] = leaf->records[i - 1];
		}

		// 2. 왼쪽 형제의 마지막 record를 가져옴
		int last = sibling->num_keys - 1;

		leaf->keys[0] = sibling->keys[last];
		leaf->records[0] = sibling->records[last];

		leaf->num_keys++;
		sibling->num_keys--;

		// 3. parent 구분 key 갱신
		parent->keys[leaf_index - 1] = leaf->keys[0];
	} else { // 오른쪽에서 가져오기
		// 1. 오른쪽 형제의 첫 record를 leaf끝에 추가
		int insert_index = leaf->num_keys;

		leaf->keys[insert_index] = sibling->keys[0];
		leaf->records[insert_index] = sibling->records[0];

		leaf->num_keys++;

		// 2. 오른쪽 형제의 나머지 데이터들을 왼쪽으로 이동
		for(int i = 0; i < sibling->num_keys - 1; i++) {
			sibling->keys[i] = sibling->keys[i + 1];
			sibling->records[i] = sibling->records[i + 1];
		}

		sibling->num_keys--;

		// 3. parent 구분 key 갱신
		parent->keys[leaf_index] = sibling->keys[0];
	}

	// 4. 변경된 세 페이지를 디스크에 저장
	write_page(fd, leaf);
	write_page(fd, sibling);
	write_page(fd, parent);
}

// internal page의 재분배를 위한 함수
static void redistribute_internal(
    int fd,
    page *node,
    page *sibling,
    page *parent,
    int node_index,
    bool from_left
) {
    if (from_left) {
        // 1. 현재 Internal의 Key와 Child를 오른쪽으로 이동
        for (int i = node->num_keys; i > 0; i--) {
            node->keys[i] = node->keys[i - 1];
        }

        for (int i = node->num_keys + 1; i > 0; i--) {
            node->child_pgns[i] = node->child_pgns[i - 1];
        }

        // 2. Parent의 구분 Key를 현재 Internal로 내림
        node->keys[0] = parent->keys[node_index - 1];

        // 3. 왼쪽 형제의 마지막 Child를 가져옴
        node->child_pgns[0] =
            sibling->child_pgns[sibling->num_keys];

        // 4. 왼쪽 형제의 마지막 Key를 Parent로 올림
        parent->keys[node_index - 1] =
            sibling->keys[sibling->num_keys - 1];

        node->num_keys++;
        sibling->num_keys--;

        // 5. 이동한 Child의 Parent 변경
        page child;
        load_page(fd, node->child_pgns[0], &child);
        child.parent_pgn = node->pgn;
        write_page(fd, &child);

    } else {
        // 1. Parent의 구분 Key를 현재 Internal 끝에 추가
        node->keys[node->num_keys] =
            parent->keys[node_index];

        // 2. 오른쪽 형제의 첫 Child를 가져옴
        int64_t moved_child =
            sibling->child_pgns[0];

        node->child_pgns[node->num_keys + 1] =
            moved_child;

        // 3. 오른쪽 형제의 첫 Key를 Parent로 올림
        parent->keys[node_index] = sibling->keys[0];

        node->num_keys++;

        // 4. 오른쪽 형제의 Key와 Child를 왼쪽으로 이동
        for (int i = 0; i < sibling->num_keys - 1; i++) {
            sibling->keys[i] = sibling->keys[i + 1];
        }

        for (int i = 0; i < sibling->num_keys; i++) {
            sibling->child_pgns[i] =
                sibling->child_pgns[i + 1];
        }

        sibling->num_keys--;

        // 5. 이동한 Child의 Parent 변경
        page child;
        load_page(fd, moved_child, &child);
        child.parent_pgn = node->pgn;
        write_page(fd, &child);
    }

    // 6. 변경 사항 저장
    write_page(fd, node);
    write_page(fd, sibling);
    write_page(fd, parent);
}

// 두 internal page를 병합하는 함수
static void coalesce_internal(
    int fd,
    page *left,
    page *right,
    int64_t separator_key
) {
    int start = left->num_keys;

    // 1. Parent의 구분 Key를 왼쪽 Internal에 추가
    left->keys[start] = separator_key;

    // 2. 오른쪽 Internal의 Key들을 복사
    for (int i = 0; i < right->num_keys; i++) {
        left->keys[start + 1 + i] = right->keys[i];
    }

    // 3. 오른쪽 Internal의 Child들을 복사
    for (int i = 0; i <= right->num_keys; i++) {
        int64_t child_pgn = right->child_pgns[i];

        left->child_pgns[start + 1 + i] = child_pgn;

        // 이동한 Child의 Parent 수정
        page child;
        load_page(fd, child_pgn, &child);
        child.parent_pgn = left->pgn;
        write_page(fd, &child);
    }

    // 4. Key 개수 갱신
    left->num_keys += right->num_keys + 1;

    // 5. 병합 결과 저장
    write_page(fd, left);

    // 6. 오른쪽 Internal Page 반환
    free_page(fd, right->pgn);
}

// 재귀적으로 internal 함수를 재분배, 병합하는 함수
static void repair_internal(
    int fd,
    header_page *header,
    int64_t node_pgn
) {
    page node;
    load_page(fd, node_pgn, &node);

    // 1. Root인 경우
    if (node.pgn == header->root_pgn) {
        adjust_root_after_delete(fd, header, &node);
        return;
    }

    // 2. 최소 Key 개수 확인
    int min_keys = (header->internal_order + 1) / 2 - 1;

    if (node.num_keys >= min_keys) {
        return;
    }

    // 3. Parent 읽기
    page parent;
    load_page(fd, node.parent_pgn, &parent);

    // Parent에서 현재 Internal의 위치 찾기
    int node_index = 0;

    while (node_index <= parent.num_keys &&
           parent.child_pgns[node_index] != node.pgn) {
        node_index++;
    }

    if (node_index > parent.num_keys) {
        exit_with_err_msg("Internal node not found in parent");
    }

    // 4. 왼쪽 형제에게서 빌릴 수 있는지 확인
    if (node_index > 0) {
        page left_sibling;

        load_page(
            fd,
            parent.child_pgns[node_index - 1],
            &left_sibling
        );

        if (left_sibling.num_keys > min_keys) {
            redistribute_internal(
                fd, &node, &left_sibling,
                &parent, node_index, true
            );
            return;
        }
    }

    // 5. 오른쪽 형제에게서 빌릴 수 있는지 확인
    if (node_index < parent.num_keys) {
        page right_sibling;

        load_page(
            fd,
            parent.child_pgns[node_index + 1],
            &right_sibling
        );

        if (right_sibling.num_keys > min_keys) {
            redistribute_internal(
                fd, &node, &right_sibling,
                &parent, node_index, false
            );
            return;
        }
    }

    // 6. 재분배가 불가능하면 병합
    if (node_index > 0) {
        // 왼쪽 형제에 현재 Internal을 병합
        page left_sibling;

        load_page(
            fd,
            parent.child_pgns[node_index - 1],
            &left_sibling
        );

        int64_t separator_key =
            parent.keys[node_index - 1];

        coalesce_internal(
            fd, &left_sibling, &node, separator_key
        );

        remove_from_internal(
            fd, &parent, node_index - 1
        );

    } else {
        // 오른쪽 형제를 현재 Internal에 병합
        page right_sibling;

        load_page(
            fd,
            parent.child_pgns[node_index + 1],
            &right_sibling
        );

        int64_t separator_key =
            parent.keys[node_index];

        coalesce_internal(
            fd, &node, &right_sibling, separator_key
        );

        remove_from_internal(
            fd, &parent, node_index
        );
    }

    // 7. Parent도 Key가 부족해졌을 수 있으므로 재귀 호출
    repair_internal(fd, header, parent.pgn);
}

// 서브트리의 최소 Key를 구하고
// 모든 Internal Page의 구분 Key를 갱신한다.
//
// 반환값:
// true  : 서브트리에 Key가 존재함
// false : 서브트리가 비어 있음
static bool refresh_separator_keys(
    int fd,
    int64_t pgn,
    int64_t *min_key
) {
    page node;
    load_page(fd, pgn, &node);

    // 1. Leaf Page인 경우
    if (node.is_leaf) {
        if (node.num_keys == 0) {
            return false;
        }

        *min_key = node.keys[0];
        return true;
    }

    // 2. Internal Page인 경우
    int64_t child_min;

    // 첫 번째 Child의 최소 Key가
    // 현재 서브트리 전체의 최소 Key
    if (!refresh_separator_keys(
            fd, node.child_pgns[0], &child_min)) {
        return false;
    }

    *min_key = child_min;

    // 3. 나머지 Child의 최소 Key를 구분 Key에 반영
    bool changed = false;

    for (int i = 1; i <= node.num_keys; i++) {
        if (!refresh_separator_keys(
                fd, node.child_pgns[i], &child_min)) {
            return false;
        }

        if (node.keys[i - 1] != child_min) {
            node.keys[i - 1] = child_min;
            changed = true;
        }
    }

    // 4. 변경된 Internal Page 저장
    if (changed) {
        write_page(fd, &node);
    }

    return true;
}


static void refresh_tree_after_delete(int fd) {
    header_page header;
    load_header_page(fd, &header);

    // 빈 트리라면 갱신할 것이 없음
    if (header.root_pgn == -1) {
        return;
    }

    int64_t min_key;

    if (!refresh_separator_keys(
            fd, header.root_pgn, &min_key)) {
        exit_with_err_msg(
            "Empty subtree after deletion"
        );
    }
}

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
