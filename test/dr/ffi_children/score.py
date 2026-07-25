# Written once by `dragon ffi sync` - this file is yours; edit freely.
from score_stub import User, Scored, serve


def score(batch):
    return [Scored(id=u.id, score=u.id * 2 + len(u.tag)) for u in batch]


serve(score)
