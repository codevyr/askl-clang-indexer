struct value {
    int v;
    bool operator!=(const value &other) const { return v != other.v; }
    value &operator=(const value &other) { v = other.v; return *this; }
};

struct holder {
    value held;
    int (*cb)(int);
};

bool differs(holder *h, const value &other)
{
    return h->held != other;
}

int invoke(holder *h)
{
    return h->cb(1);
}
