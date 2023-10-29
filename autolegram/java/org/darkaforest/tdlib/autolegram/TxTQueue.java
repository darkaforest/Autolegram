package org.darkaforest.tdlib.autolegram;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.HashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Queue;
import java.util.Set;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

public final class TxTQueue {

    private final Lock lock = new ReentrantLock();

    private final Queue<String> innerQueue = new LinkedList<>();

    private Path txtPath;

    private final Set<String> markedStrSet = new HashSet<>();

    TxTQueue() {
    }

   public void init(String path) throws IOException {
        createDir(new File(path).getParent());
        this.txtPath = Paths.get(path);
        if (!Files.exists(txtPath)) {
            Files.createFile(txtPath);
        } else {
            List<String> strs = Files.readAllLines(txtPath);
            if (strs.isEmpty()) {
                return;
            }
            for (String str : strs) {
                if (str == null || str.isEmpty()) {
                    continue;
                }
                char mark = str.charAt(0);
                if (mark == '+') {
                    markedStrSet.add(str.substring(1));
                } else if (mark == '-') {
                    markedStrSet.remove(str.substring(1));
                } else {
                    continue;
                }
            }
            Files.delete(txtPath);
            Files.createFile(txtPath);
            for (String str : markedStrSet) {
                innerQueue.offer(str);
                Files.write(txtPath, ("+" + str + "\n").getBytes(), StandardOpenOption.APPEND);
            }
        }
    }

    public boolean isEmpty() {
        return innerQueue.isEmpty();
    }

    public int size() {
        return innerQueue.size();
    }

    private static void createDir(String dir) {
        try {
            Path path = Paths.get(dir);
            if (Files.notExists(path)) {
                Files.createDirectories(path);
            }
        } catch (Exception e) {
        }
    }

    public String peek() {
        try {
            lock.lock();
            return innerQueue.peek();
        } finally {
            lock.unlock();
        }
    }

    public String poll() {
        try {
            lock.lock();
            String t = innerQueue.poll();
            if (t == null) {
                return null;
            }
            markedStrSet.remove(t);
            Files.write(txtPath, ("-" + t + "\n").getBytes(), StandardOpenOption.APPEND);
            return t;
        } catch (Exception ignored) {
            return null;
        } finally {
            lock.unlock();
        }
    }

    public void offer(String str) {
        try {
            lock.lock();
            if (markedStrSet.contains(str)) {
                return;
            }
            markedStrSet.add(str);
            innerQueue.offer(str);
            Files.write(txtPath, ("+" + str + "\n").getBytes(), StandardOpenOption.APPEND);
        } catch (Exception ignored) {
        } finally {
            lock.unlock();
        }
    }
}
