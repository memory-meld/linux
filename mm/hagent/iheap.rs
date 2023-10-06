#![allow(unsafe_op_in_unsafe_fn)]

use crate::alloc::*;
use hashbrown::HashMap;
use kernel::prelude::*;

use core::{
    cmp,
    hash::{BuildHasherDefault, Hash, Hasher},
    mem::{self, ManuallyDrop},
    ptr,
};

pub struct IHeap<K, V, A: Allocator = Global> {
    index: HashMap<K, usize, BuildHasherDefault<FnvHasher>, A>,
    data: Vec<(K, V), A>,
}

impl<K: Eq + Hash + Clone, V: Ord> IHeap<K, V, Global> {
    #[must_use]
    pub fn new() -> Self {
        Self::new_in(Global)
    }

    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self::with_capacity_in(capacity, Global)
    }
}

impl<K: Eq + Hash + Clone, V: Ord, A: Allocator + Clone> IHeap<K, V, A> {
    #[must_use]
    pub fn new_in(alloc: A) -> Self {
        Self {
            index: HashMap::with_hasher_in(
                BuildHasherDefault::<FnvHasher>::default(),
                alloc.clone(),
            ),
            data: Vec::new_in(alloc),
        }
    }

    #[must_use]
    pub fn with_capacity_in(capacity: usize, alloc: A) -> Self {
        Self {
            index: HashMap::with_capacity_and_hasher_in(
                capacity,
                BuildHasherDefault::<FnvHasher>::default(),
                alloc.clone(),
            ),
            data: Vec::try_with_capacity_in(capacity, alloc).unwrap(),
        }
    }

    pub fn pop(&mut self) -> Option<(K, V)> {
        self.data.pop().map(|mut item| {
            if !self.is_empty() {
                mem::swap(&mut item, &mut self.data[0]);
                // SAFETY: !self.is_empty() means that self.len() > 0
                unsafe { self.sift_down(0, self.len()) };
            }
            let old = self.index.remove(&item.0);
            debug_assert!(old.is_some());
            item
        })
    }

    pub fn push(&mut self, item: (K, V)) {
        let pos = self.len();
        let old = self.index.insert(item.0.clone(), pos);
        debug_assert!(old.is_none());
        self.data.try_push(item).unwrap();
        // SAFETY: Since we pushed a new item it means that
        //  old_len = self.len() - 1 < self.len()
        unsafe { self.sift_up(0, pos) };
    }

    pub fn get(&self, key: &K) -> Option<&usize> {
        self.index.get(key)
    }

    pub fn remove(&mut self, pos: usize) -> Option<(K, V)> {
        let len = self.len();
        debug_assert!(pos < len);
        let end = len - 1;
        if pos < end {
            self.index.insert(self.data[end].0.clone(), pos);
            self.data.swap(pos, end);
            unsafe { self.sift_down(pos, end) };
        }
        self.index.remove(&self.data[end].0);
        self.data.pop()
    }

    pub fn update(&mut self, pos: usize, new_value: V) {
        debug_assert!(pos < self.len());
        let (_, v) = &mut self.data[pos];
        // min heap
        match new_value.cmp(v) {
            cmp::Ordering::Less => {
                *v = new_value;
                unsafe { self.sift_up(0, pos) };
            }
            cmp::Ordering::Equal => {}
            cmp::Ordering::Greater => {
                *v = new_value;
                unsafe { self.sift_down(pos, self.len()) };
            }
        }
    }

    pub fn replace(&mut self, pos: usize, item: (K, V)) -> (K, V) {
        let mut item = item;
        let len = self.len();
        debug_assert!(pos < len);
        let dest = &mut self.data[pos];
        self.index.remove(&dest.0);
        mem::swap(&mut item, dest);
        self.index.insert(dest.0.clone(), pos);
        match item.1.cmp(&dest.1) {
            cmp::Ordering::Less => {
                // min heap, new item has a larger value
                unsafe { self.sift_down(pos, len) };
            }
            cmp::Ordering::Equal => {}
            cmp::Ordering::Greater => {
                // min heap, new item has a smaller value
                unsafe { self.sift_up(0, pos) };
            }
        }
        item
    }

    // The implementations of sift_up and sift_down use unsafe blocks in
    // order to move an element out of the vector (leaving behind a
    // hole), shift along the others and move the removed element back into the
    // vector at the final location of the hole.
    // The `Hole` type is used to represent this, and make sure
    // the hole is filled back at the end of its scope, even on panic.
    // Using a hole reduces the constant factor compared to using swaps,
    // which involves twice as many moves.

    /// # Safety
    ///
    /// The caller must guarantee that `pos < self.len()`.
    unsafe fn sift_up(&mut self, begin: usize, pos: usize) -> usize {
        // Take out the value at `pos` and create a hole.
        // SAFETY: The caller guarantees that pos < self.len()
        let mut hole = self.hole_at(pos);

        while hole.pos() > begin {
            let parent = (hole.pos() - 1) / 2;

            // SAFETY: hole.pos() > start >= 0, which means hole.pos() > 0
            //  and so hole.pos() - 1 can't underflow.
            //  This guarantees that parent < hole.pos() so
            //  it's a valid index and also != hole.pos().
            if hole.element().1 >= hole.get(parent).1 {
                break;
            }

            // SAFETY: Same as above
            hole.move_to(parent);
        }

        hole.pos()
    }

    /// Take an element at `pos` and move it down the heap,
    /// while its children are larger.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that `pos < end <= self.len()`.
    unsafe fn sift_down(&mut self, pos: usize, end: usize) -> usize {
        // SAFETY: The caller guarantees that pos < end <= self.len().
        let mut hole = self.hole_at(pos);
        let mut child = 2 * hole.pos() + 1;

        // Loop invariant: child == d * hole.pos() + 1.
        while child < end {
            // compare with the greatest of the d children
            // SAFETY: child < end - d + 1 < self.len() and
            //  child + d - 1 < end <= self.len(), so they're valid indexes.
            //  child + i == d * hole.pos() + 1 + i != hole.pos() for i >= 0
            child = hole.min_sibling(child, end);

            // if we are already in order, stop.
            // SAFETY: child is now either the old child or valid sibling
            //  We already proven that all are < self.len() and != hole.pos()
            if hole.element().1 <= hole.get(child).1 {
                break;
            }

            // SAFETY: same as above.
            hole.move_to(child);
            child = 2 * hole.pos() + 1;
        }

        hole.pos()
    }

    unsafe fn hole_at(&mut self, pos: usize) -> Hole<'_, K, V, A> {
        Hole::new(self, pos)
    }
}

/// Hole represents a hole in a slice i.e., an index without valid value
/// (because it was moved from or duplicated).
/// In drop, `Hole` will restore the slice by filling the hole
/// position with the value that was originally removed.
struct Hole<'a, K: Eq + Hash + Clone, V, A: Allocator> {
    heap: &'a mut IHeap<K, V, A>,
    elem: ManuallyDrop<(K, V)>,
    pos: usize,
}

impl<'a, K: Eq + Hash + Clone, V, A: Allocator> Hole<'a, K, V, A> {
    /// Create a new `Hole` at index `pos`.
    ///
    /// Unsafe because pos must be within the data slice.
    #[inline]
    unsafe fn new(heap: &'a mut IHeap<K, V, A>, pos: usize) -> Self {
        debug_assert!(pos < heap.len());
        // SAFE: pos should be inside the slice
        let elt = ptr::read(heap.data.get_unchecked(pos));
        Hole {
            heap,
            elem: ManuallyDrop::new(elt),
            pos,
        }
    }

    #[inline]
    fn pos(&self) -> usize {
        self.pos
    }

    /// Returns a reference to the element removed.
    #[inline]
    fn element(&self) -> &(K, V) {
        &self.elem
    }

    /// Returns a reference to the element at `index`.
    ///
    /// Unsafe because index must be within the data slice and not equal to pos.
    #[inline]
    unsafe fn get(&self, index: usize) -> &(K, V) {
        debug_assert!(index != self.pos);
        debug_assert!(index < self.heap.len());
        self.heap.data.get_unchecked(index)
    }

    /// Move hole to new location
    ///
    /// Unsafe because index must be within the data slice and not equal to pos.
    #[inline]
    unsafe fn move_to(&mut self, index: usize) {
        debug_assert!(index != self.pos);
        debug_assert!(index < self.heap.len());
        let data = self.heap.data.as_mut_ptr();
        let elem: *const _ = data.add(index);
        let hole = data.add(self.pos);
        let old = self
            .heap
            .index
            .insert(self.heap.data.get_unchecked(index).0.clone(), self.pos);
        debug_assert!(old.is_some());
        ptr::copy_nonoverlapping(elem, hole, 1);
        self.pos = index;
    }
}

impl<'a, K: Eq + Hash + Clone, V: Ord, A: Allocator> Hole<'a, K, V, A> {
    /// Get largest element
    ///
    /// Unsafe because both elements must be within the data slice and not equal
    /// to pos.
    #[inline]
    unsafe fn min(&self, elem1: usize, elem2: usize) -> usize {
        if self.get(elem1).1 <= self.get(elem2).1 {
            elem1
        } else {
            elem2
        }
    }

    /// Get index of greatest sibling
    ///
    /// Unsafe because all siblings must be within the data slice and not equal
    /// to pos.
    /// 0 < pos < end must hold
    #[inline]
    unsafe fn min_sibling(&self, left: usize, end: usize) -> usize {
        // left < end must hold
        let right = left + 1;
        if right >= end {
            return left;
        }
        self.min(left, right)
    }
}

impl<K: Eq + Hash + Clone, V, A: Allocator> Drop for Hole<'_, K, V, A> {
    #[inline]
    fn drop(&mut self) {
        // fill the hole again
        unsafe {
            let pos = self.pos;
            ptr::copy_nonoverlapping(&*self.elem, self.heap.data.get_unchecked_mut(pos), 1);
            let old = self
                .heap
                .index
                .insert(self.heap.data.get_unchecked(pos).0.clone(), pos);
            debug_assert!(old.is_some());
        }
    }
}

impl<K, V, A: Allocator> IHeap<K, V, A> {
    pub fn peek(&self) -> Option<&(K, V)> {
        self.data.get(0)
    }

    pub fn capacity(&self) -> usize {
        self.data.capacity()
    }

    pub fn reserve(&mut self, additional: usize) {
        self.data.try_reserve(additional).unwrap()
    }

    pub fn as_slice(&self) -> &[(K, V)] {
        self.data.as_slice()
    }

    #[must_use = "`self` will be dropped if the result is not used"]
    pub fn into_vec(self) -> Vec<(K, V), A> {
        self.into()
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn data(&self) -> &[(K, V)] {
        &self.data
    }
}

impl<K, V, A: Allocator> From<IHeap<K, V, A>> for Vec<(K, V), A> {
    /// Converts a `DaryHeap<T, D>` into a `Vec<T>`.
    ///
    /// This conversion requires no data movement or allocation, and has
    /// constant time complexity.
    fn from(heap: IHeap<K, V, A>) -> Vec<(K, V), A> {
        heap.data
    }
}

pub struct FnvHasher(u64);

impl Default for FnvHasher {
    #[inline]
    fn default() -> FnvHasher {
        FnvHasher(0xcbf29ce484222325)
    }
}

impl FnvHasher {
    /// Create an FNV hasher starting with a state corresponding
    /// to the hash `key`.
    #[inline]
    pub fn with_key(key: u64) -> FnvHasher {
        FnvHasher(key)
    }
}

impl Hasher for FnvHasher {
    #[inline]
    fn finish(&self) -> u64 {
        self.0
    }

    #[inline]
    fn write(&mut self, bytes: &[u8]) {
        let FnvHasher(mut hash) = *self;

        for byte in bytes.iter() {
            hash = hash ^ (*byte as u64);
            hash = hash.wrapping_mul(0x100000001b3);
        }

        *self = FnvHasher(hash);
    }
}

/// A builder for default FNV hashers.
pub type FnvBuildHasher = BuildHasherDefault<FnvHasher>;
