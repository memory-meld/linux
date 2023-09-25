#include <assert.h>

#include "fifo.h"
#include "hemem.h"
#include "log.h"
#include "unwrap.h"

// insert to the head
void enqueue_fifo(struct fifo_list *queue, struct hemem_page *entry)
{
	UNWRAP_ERR(pthread_mutex_lock(&(queue->list_lock)));
	assert(entry->prev == NULL);
	entry->next = queue->first;
	if (queue->first != NULL) {
		assert(queue->first->prev == NULL);
		queue->first->prev = entry;
	} else {
		assert(queue->last == NULL);
		assert(queue->numentries == 0);
		queue->last = entry;
	}

	queue->first = entry;
	entry->list = queue;
	queue->numentries++;
	UNWRAP_ERR(pthread_mutex_unlock(&(queue->list_lock)));
}

struct hemem_page *dequeue_fifo(struct fifo_list *queue)
{
	UNWRAP_ERR(pthread_mutex_lock(&(queue->list_lock)));
	struct hemem_page *ret = queue->last;

	if (ret == NULL) {
		//assert(queue->numentries == 0);
		UNWRAP_ERR(pthread_mutex_unlock(&(queue->list_lock)));
		return ret;
	}

	queue->last = ret->prev;
	if (queue->last != NULL) {
		queue->last->next = NULL;
	} else {
		queue->first = NULL;
	}

	ret->prev = ret->next = NULL;
	ret->list = NULL;
	assert(queue->numentries > 0);
	queue->numentries--;
	UNWRAP_ERR(pthread_mutex_unlock(&(queue->list_lock)));

	return ret;
}

void page_list_remove_page(struct fifo_list *list, struct hemem_page *page)
{
	UNWRAP_ERR(pthread_mutex_lock(&(list->list_lock)));
	if (list->first == NULL) {
		assert(list->last == NULL);
		assert(list->numentries == 0);
		UNWRAP_ERR(pthread_mutex_unlock(&(list->list_lock)));
		LOG("page_list_remove_page: list was empty!\n");
		return;
	}

	if (list->first == page) {
		list->first = page->next;
	}

	if (list->last == page) {
		list->last = page->prev;
	}

	if (page->next != NULL) {
		page->next->prev = page->prev;
	}

	if (page->prev != NULL) {
		page->prev->next = page->next;
	}

	assert(list->numentries > 0);
	list->numentries--;
	page->next = NULL;
	page->prev = NULL;
	page->list = NULL;
	UNWRAP_ERR(pthread_mutex_unlock(&(list->list_lock)));
}

struct hemem_page *next_page(struct fifo_list *list, struct hemem_page *page)
{
	struct hemem_page *next_page = NULL;

	UNWRAP_ERR(pthread_mutex_lock(&(list->list_lock)));
	if (page == NULL) {
		next_page = list->last;
	} else {
		next_page = page->prev;
		assert(page->list == list);
	}
	UNWRAP_ERR(pthread_mutex_unlock(&(list->list_lock)));

	return next_page;
}

// iterate from the tail to the head which follows the fifo semantics
ulong fifo_for_each(struct fifo_list *list, void (*f)(struct hemem_page *))
{
	ulong done = 0;
	UNWRAP_ERR(pthread_mutex_lock(&(list->list_lock)));
	for (struct hemem_page *p = list->last; p; p = p->prev, ++done)
		f(p);
	UNWRAP_ERR(pthread_mutex_unlock(&(list->list_lock)));
	return done;
}
