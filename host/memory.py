"""Local memory adapted from edw-324/dafeiyu-desktop-pet, pet/state.py.

The upstream code is MIT licensed. This port uses SQLite and requires an
explicit request to create a lasting memory.
"""

import sqlite3
from contextlib import closing


PRIVATE_TERMS = ("密码", "验证码", "银行卡", "身份证", "私钥", "token", "api key", "apikey", "密钥")
NEGATIVE_TERMS = ("忘记", "清空记忆", "别记", "不要记")
EXPLICIT_TERMS = ("记住", "记一下", "记下来", "别忘", "记得我")
PREFERENCE_TERMS = ("我喜欢", "我不喜欢", "我爱", "我讨厌", "我受不了", "我怕", "我不吃", "我吃不了", "我对", "过敏")


def memory_from_text(text):
    text = " ".join(text.split())
    lower = text.lower()
    if not 4 <= len(text) <= 120 or any(word in lower for word in PRIVATE_TERMS + NEGATIVE_TERMS):
        return None
    if not any(word in text for word in EXPLICIT_TERMS):
        return None
    kind = "preferences" if any(word in text for word in PREFERENCE_TERMS) else "facts"
    return kind, text[:100]


class Memory:
    def __init__(self, path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        with closing(self.connect()) as db, db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS history (
                    id INTEGER PRIMARY KEY, prompt TEXT NOT NULL, reply TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS notes (
                    id INTEGER PRIMARY KEY, kind TEXT NOT NULL, text TEXT NOT NULL UNIQUE
                );
                CREATE TABLE IF NOT EXISTS revision (
                    id INTEGER PRIMARY KEY CHECK (id = 1), number INTEGER NOT NULL
                );
                INSERT OR IGNORE INTO revision VALUES (1, 0);
            """)

    def connect(self):
        return sqlite3.connect(self.path, timeout=5)

    def snapshot(self):
        with closing(self.connect()) as db, db:
            db.execute("BEGIN")
            revision = db.execute("SELECT number FROM revision").fetchone()[0]
            history = []
            for prompt, reply in db.execute("SELECT prompt, reply FROM history ORDER BY id"):
                history.extend(({"role": "user", "content": prompt},
                                {"role": "assistant", "content": reply}))
            notes = [text for (text,) in db.execute("SELECT text FROM notes ORDER BY id")]
            return revision, history, notes

    def notes(self):
        return self.snapshot()[2]

    def remember(self, prompt, reply, revision):
        candidate = memory_from_text(prompt)
        with closing(self.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            if revision != db.execute("SELECT number FROM revision").fetchone()[0]:
                return
            if not any(word in (prompt + " " + reply).lower() for word in PRIVATE_TERMS):
                db.execute("INSERT INTO history(prompt, reply) VALUES (?, ?)", (prompt, reply))
                db.execute("DELETE FROM history WHERE id NOT IN (SELECT id FROM history ORDER BY id DESC LIMIT 6)")
            if candidate:
                db.execute("INSERT OR IGNORE INTO notes(kind, text) VALUES (?, ?)", candidate)
                db.execute("""DELETE FROM notes WHERE kind = ? AND id NOT IN (
                    SELECT id FROM notes WHERE kind = ? ORDER BY id DESC LIMIT 12
                )""", (candidate[0], candidate[0]))

    def clear(self):
        with closing(self.connect()) as db, db:
            db.execute("DELETE FROM history")
            db.execute("DELETE FROM notes")
            db.execute("UPDATE revision SET number = number + 1")
