

## Phase 1: Object Storage Foundation

### Screenshot 1A: Output of ./test_objects showing all tests passing.

![1A: Object Storage Tests](./screenshots/1a.png)

### Screenshot 1B: find .pes/objects -type f showing the sharded directory structure.

![1B: Sharded Object Directory Structure](./screenshots/1b.png)

### Summary
Object storage implementation complete. The `object_write` and `object_read` functions successfully:
- Store objects with type headers (blob, tree, commit)
- Compute SHA-256 hashes for content addressing
- Shard objects into subdirectories by first 2 hex characters
- Verify integrity on retrieval

---

## Phase 2: Tree Objects

### Screenshot 2A: Output of ./test_tree showing all tests passing.

![2A: Tree Serialization Tests](./screenshots/2a.jpeg)

### Screenshot 2B: Pick a tree object from find .pes/objects -type f and run xxd .pes/objects/XX/YYY... | head -20 to show the raw binary format.

![2B: Raw Tree Object Format](./screenshots/2b.png)

### Summary
Tree object implementation complete. The `tree_from_index` function:
- Builds hierarchical tree structures from index entries
- Handles nested paths and creates intermediate directories
- Writes all tree objects to the object store
- Returns root tree hash for commits

---

## Phase 3: The Index (Staging Area)

### Screenshot 3A: Run ./pes init, ./pes add file1.txt file2.txt, ./pes status — show the output.

![3A: Index and Status Workflow](./screenshots/3a.jpeg)

### Screenshot 3B: cat .pes/index showing the text-format index with your entries.

![3B: Index File Format](./screenshots/3b.jpeg)

### Summary
Index implementation complete. Functions implemented:
- `index_load`: Reads text-based index file, handles missing file gracefully
- `index_save`: Atomically writes index entries sorted by path
- `index_add`: Stages files by computing blob hash and updating index entry

---

## Phase 4: Commits and History

### Screenshot 4A: Output of ./pes log showing three commits with hashes, authors, timestamps, and messages.

![4A: Commit Log](./screenshots/4a.jpeg)

### Screenshot 4B: find .pes -type f | sort showing object store growth after three commits.

![4B: Object Store Growth](./screenshots/4b.jpeg)

### Screenshot 4C: cat .pes/refs/heads/main and cat .pes/HEAD showing the reference chain.

![4C: Reference Chain](./screenshots/4c.jpeg)

### Summary
Commit implementation complete. The `commit_create` function:
- Builds tree from staged changes
- Reads parent commit from HEAD
- Writes commit object with metadata (author, timestamp, message)
- Updates branch reference to new commit

---



# OS - ORANGE PROBLEM REPORT

# Building PES-VCS — A Version Control System from Scratch

**NAME:** ANCHITA CHHIBBA  
**SRN:** PES2UG24CS060  

---

## Phase 5 & 6: Analysis-Only Questions

# Q5.1 — Branching and Checkout

In this system, a branch is simply a file inside `.pes/refs/heads/` that stores a commit hash. So implementing `pes checkout <branch>` mainly involves updating references and synchronizing the working directory.

First, the program must locate the file `.pes/refs/heads/<branch>`. If it does not exist, checkout should fail. If it exists, the commit hash inside that file becomes the target commit.

Next, the `HEAD` file must be updated. Instead of pointing to the current branch, it should now contain:

```

ref: refs/heads/<branch>

```

This effectively switches the current branch.

After updating `HEAD`, the working directory must be updated to match the tree of the target commit. This means reading the commit object, getting its tree, and then recreating all files and directories from that tree into the working directory. Existing tracked files may need to be overwritten, deleted, or created depending on differences.

This operation becomes complex because it is not just changing a pointer. The working directory must exactly match the snapshot stored in the commit. That involves recursively walking trees, handling file modes, deleting files that no longer exist, and ensuring no uncommitted changes are lost. Handling conflicts and preserving user data makes checkout much more complicated than simply switching a file reference.

---

# Q5.2 — Detecting Dirty Working Directory Conflicts

When switching branches, the system must ensure that no uncommitted changes are overwritten. This is especially important when a tracked file has been modified locally and also differs in the target branch.

To detect this, the index and object store can be used. The index represents the last staged version of files, and each entry contains the blob hash of the file content. For each tracked file, the system can compare the current working directory file with the corresponding blob stored in the index. If the file’s current content does not match the blob hash in the index, then the file has been modified and is considered “dirty”.

At the same time, the system must check what the target branch’s commit contains. If the same file exists in the target commit but has a different blob hash than the current index, then switching branches would overwrite the user’s local changes.

So the conflict condition is: a file is modified in the working directory (compared to the index), and that same file is different in the target branch. If this condition is detected, checkout should refuse and warn the user.

This approach avoids directly comparing with the working directory snapshot of the current branch and instead relies only on the index and stored objects, which are reliable and already hashed.

---

# Q5.3 — Detached HEAD

A detached `HEAD` state occurs when the `HEAD` file contains a commit hash directly instead of pointing to a branch reference. In this case, new commits are still created normally, but they are not attached to any branch.

If a user makes commits in this state, each new commit will point to the previous one as its parent, forming a chain. However, since no branch reference points to these commits, they are effectively “floating” and can become unreachable.

If the user switches to another branch without saving these commits, they may eventually be lost during garbage collection.

To recover such commits, the user can create a new branch pointing to the current commit before switching away. This can be done by writing the current commit hash into a new file inside `.pes/refs/heads/`. As long as the commit hash is known (for example from logs or terminal output), it can be restored by manually creating a branch reference to it.

---

# Q6.1 — Garbage Collection and Reachability

Over time, many objects accumulate in the object store that are no longer referenced by any branch. To clean these up, we need to identify which objects are still reachable.

The process starts from all branch heads, which are stored in `.pes/refs/heads/`. Each branch points to a commit. From each commit, we recursively follow parent links to visit all ancestor commits. For every commit, we also read its tree object, and recursively traverse all subtrees and blobs referenced by it.

While traversing, we maintain a set of visited object hashes. A hash set is ideal here because it allows constant-time lookup and avoids visiting the same object multiple times.

Once traversal is complete, we scan the entire object store. Any object whose hash is not in the reachable set can be safely deleted.

For a repository with around 100,000 commits and 50 branches, the traversal will not visit 50 × 100,000 commits independently because branches often share history. In practice, most commits overlap, so the total number of unique commits visited will be close to 100,000. In addition, each commit references one tree, and trees reference blobs and subtrees, so the total number of objects visited could be several times higher depending on repository size.

---

# Q6.2 — Garbage Collection Race Condition

Running garbage collection at the same time as a commit operation is dangerous because of a possible race condition.

Consider a situation where a commit is being created. First, the system writes new tree and blob objects into the object store. Then it creates the commit object referencing those trees and blobs. Finally, it updates the branch reference to point to the new commit.

If garbage collection runs in between these steps, it might scan the object store and not find any references to the newly written objects yet, since the commit object has not been fully written or the branch has not been updated. As a result, GC may incorrectly consider those objects unreachable and delete them.

After that, when the commit finishes and tries to reference those objects, they no longer exist, leading to corruption.

Real Git avoids this problem using techniques like locking and staging. Objects are first written safely, and references are updated atomically. During garbage collection, Git uses mechanisms such as lock files and temporary object directories to ensure it does not delete objects that are in the process of being created. It also relies on the fact that references are updated only after all objects are safely written, reducing the window where such race conditions can occur.
```

---


### Conflict condition:

A checkout must be refused if:

```c
file is modified locally (differs from index)
AND
file differs between current and target commit