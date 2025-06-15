Each layer must implement the following methods:
* `int send(void *storage, Buffer input, Buffer* output)`
* `int recv(void *storage, Buffer input, Buffer* output)`
* `DataLayer *init()`

The `init()` method must create a new DataLayer with pointers to the `send()` and `recv()` methods.
Both `send()` and `recv()` must:
* Accept an `input` of size 0
* Return the number of *input* bytes which were processed and can now be discarded (not necessarily the number of bytes output)
* Set the `output` Buffer's `size` and `data` fields to the resultant data (this is where the number of bytes output goes)