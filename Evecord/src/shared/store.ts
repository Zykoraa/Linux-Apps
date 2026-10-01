/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

type Listener<V = any> = (value: V) => void;
type AnyListener<T> = (data: T, path: string) => void;

/**
 * A plain object wrapped in a proxy that reports every write.
 *
 * `store` is the observed view: assigning to `store.a.b` notifies listeners on
 * "a.b" and every "any" listener. `data` is the same object without the proxy,
 * for serialising or sending over IPC.
 */
export class Store<T extends object> {
    private pathListeners = new Map<string, Set<Listener>>();
    private anyListeners = new Set<AnyListener<T>>();

    declare data: T;
    declare store: T;

    constructor(data: T) {
        this.replace(data);
    }

    private wrap(target: any, path: string): any {
        return new Proxy(target, {
            get: (obj, key) => {
                const value = obj[key];
                if (typeof key === "string" && value !== null && typeof value === "object" && !Array.isArray(value)) {
                    return this.wrap(value, join(path, key));
                }
                return value;
            },
            set: (obj, key, value) => {
                if (obj[key] === value) return true;
                obj[key] = value;
                if (typeof key === "string") this.emit(join(path, key), value);
                return true;
            },
            deleteProperty: (obj, key) => {
                if (!(key in obj)) return true;
                delete obj[key];
                if (typeof key === "string") this.emit(join(path, key), undefined);
                return true;
            }
        });
    }

    private emit(path: string, value: unknown) {
        for (const cb of this.anyListeners) cb(this.data, path);
        this.pathListeners.get(path)?.forEach(cb => cb(value));
    }

    /**
     * Swap in a whole new object, e.g. one that arrived over IPC. "Any" listeners fire
     * with an empty path; path listeners do not, since nothing says which paths moved.
     */
    replace(data: T) {
        this.data = data;
        this.store = this.wrap(data, "");
        for (const cb of this.anyListeners) cb(data, "");
    }

    onChange(path: string, cb: Listener) {
        let set = this.pathListeners.get(path);
        if (!set) this.pathListeners.set(path, (set = new Set()));
        set.add(cb);
        return () => set.delete(cb);
    }

    onAnyChange(cb: AnyListener<T>) {
        this.anyListeners.add(cb);
        return () => this.anyListeners.delete(cb);
    }
}

function join(path: string, key: string) {
    return path ? `${path}.${key}` : key;
}
