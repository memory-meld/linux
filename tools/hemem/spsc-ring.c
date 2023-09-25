#include <stdlib.h>
#include <assert.h>

#include "spsc-ring.h"

struct ring_buf_t {
	ring_buf_elem_t *buffer;
	size_t head;
	size_t tail;
	size_t capacity;
};

static void advance_pointer(ring_handle_t rbuf)
{
	assert(rbuf);

	if (ring_buf_full(rbuf)) {
		if (++(rbuf->tail) == rbuf->capacity) {
			rbuf->tail = 0;
		}
	}

	if (++(rbuf->head) == rbuf->capacity) {
		rbuf->head = 0;
	}
}

static void retreat_pointer(ring_handle_t rbuf)
{
	assert(rbuf);

	if (++(rbuf->tail) == rbuf->capacity) {
		rbuf->tail = 0;
	}
}

ring_handle_t ring_buf_init(ring_buf_elem_t *buffer, size_t size)
{
	assert(buffer && size);

	ring_handle_t rbuf = calloc(1, sizeof(ring_buf_t));
	assert(rbuf);
	struct ring_buf_t new = {
		.buffer = buffer,
		.capacity = size,
		.head = 0,
		.tail = 0,
	};
	*rbuf = new;
	assert(ring_buf_empty(rbuf));
	return rbuf;
}

void ring_buf_free(ring_handle_t rbuf)
{
	assert(rbuf);
	free(rbuf);
}

// void ring_buf_reset(ring_handle_t rbuf)
// {
// 	assert(rbuf);

// 	rbuf->head = 0;
// 	rbuf->tail = 0;
// }

size_t ring_buf_size(ring_handle_t rbuf)
{
	assert(rbuf);

	size_t size = rbuf->capacity;

	if (!ring_buf_full(rbuf)) {
		if (rbuf->head >= rbuf->tail) {
			size = (rbuf->head - rbuf->tail);
		} else {
			size = (rbuf->capacity + rbuf->head - rbuf->tail);
		}
	}

	return size;
}

size_t ring_buf_capacity(ring_handle_t rbuf)
{
	assert(rbuf);

	return rbuf->capacity;
}

void ring_buf_put(ring_handle_t rbuf, ring_buf_elem_t data)
{
	assert(rbuf && rbuf->buffer);

	rbuf->buffer[rbuf->head] = data;

	advance_pointer(rbuf);
}

int ring_buf_put2(ring_handle_t rbuf, ring_buf_elem_t data)
{
	int r = -1;

	assert(rbuf && rbuf->buffer);

	if (!ring_buf_full(rbuf)) {
		rbuf->buffer[rbuf->head] = data;
		advance_pointer(rbuf);
		r = 0;
	}

	return r;
}

ring_buf_elem_t ring_buf_get(ring_handle_t rbuf)
{
	assert(rbuf && rbuf->buffer);
	ring_buf_elem_t cur_read;

	if (!ring_buf_empty(rbuf)) {
		cur_read = rbuf->buffer[rbuf->tail];
		retreat_pointer(rbuf);

		return cur_read;
	}

	return NULL;
}

bool ring_buf_empty(ring_handle_t rbuf)
{
	assert(rbuf);

	return (!ring_buf_full(rbuf) && (rbuf->head == rbuf->tail));
}

bool ring_buf_full(ring_buf_t *rbuf)
{
	size_t head = rbuf->head + 1;
	if (head == rbuf->capacity) {
		head = 0;
	}

	return head == rbuf->tail;
}
