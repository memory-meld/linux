use core::{
    cell::UnsafeCell,
    cmp,
    convert::AsRef,
    mem::{self, MaybeUninit},
    sync::atomic::{AtomicUsize, Ordering},
};

use alloc::collections::TryReserveError;
use kernel::{prelude::*, sync::Arc};

struct Inner<T> {
    buf: Vec<MaybeUninit<T>>,
    // next index to put new element on
    tx: AtomicUsize,
    // next index to recv element on
    rx: AtomicUsize,
}

impl<T> Inner<T> {
    fn with_capacity(cap: usize) -> Result<Self, TryReserveError> {
        let mut buf = Vec::try_with_capacity(cap)?;
        for _ in 0..cap {
            buf.try_push(MaybeUninit::uninit())?
        }
        Ok(Self {
            buf,
            tx: AtomicUsize::new(0),
            rx: AtomicUsize::new(0),
        })
    }
}

impl<T: Send> Inner<T> {
    fn send(&mut self, elem: T) -> Result<(), T> {
        // space would always be larger than zero, because rx would never exceed tx
        let tx = self.tx.load(Ordering::SeqCst);
        let len = self.buf.len();
        if tx - self.rx.load(Ordering::SeqCst) < len {
            self.buf[tx % len].write(elem);
            self.tx.fetch_add(1, Ordering::SeqCst);
            Ok(())
        } else {
            Err(elem)
        }
    }

    fn recv(&mut self) -> Option<T> {
        match self
            .rx
            .load(Ordering::SeqCst)
            .cmp(&self.tx.load(Ordering::SeqCst))
        {
            cmp::Ordering::Less => {
                let pos = self.rx.fetch_add(1, Ordering::SeqCst) % self.buf.len();
                let mut elem = MaybeUninit::uninit();
                mem::swap(&mut self.buf[pos], &mut elem);
                // SAFETY: rx < tx guarantees that the element is written by the sender
                Some(unsafe { elem.assume_init() })
            }
            cmp::Ordering::Equal => None,
            cmp::Ordering::Greater => unreachable!(),
        }
    }
}

pub struct Sender<T> {
    inner: Arc<UnsafeCell<Inner<T>>>,
}

unsafe impl<T: Send> Send for Sender<T> {}

impl<T: Send> Sender<T> {
    pub fn send(&self, elem: T) -> Result<(), T> {
        let inner = unsafe { &mut *self.inner.as_ref().get() };
        inner.send(elem)
    }
}

pub struct Receiver<T> {
    inner: Arc<UnsafeCell<Inner<T>>>,
}

unsafe impl<T: Send> Send for Receiver<T> {}

impl<T: Send> Receiver<T> {
    pub fn recv(&self) -> Option<T> {
        let inner = unsafe { &mut *self.inner.as_ref().get() };
        inner.recv()
    }
}

pub fn channel<T: Send>(cap: usize) -> (Sender<T>, Receiver<T>) {
    let inner = Arc::try_new(UnsafeCell::new(Inner::with_capacity(cap).unwrap())).unwrap();
    (
        Sender {
            inner: inner.clone(),
        },
        Receiver { inner },
    )
}
