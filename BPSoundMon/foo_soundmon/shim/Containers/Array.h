#pragma once

#include <vector>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <cstring>

namespace core {

template<typename T>
class Array {
public:
    T& Add(const T& val) { m_data.push_back(val); return m_data.back(); }

    std::pair<typename std::vector<T>::iterator, bool> AddOnce(const T& val) {
        auto it = std::find(m_data.begin(), m_data.end(), val);
        if (it != m_data.end())
            return { it, false };
        m_data.push_back(val);
        return { m_data.end() - 1, true };
    }

    size_t NumItems() const { return m_data.size(); }
    const T* Items() const { return m_data.data(); }
    size_t Size() const { return m_data.size() * sizeof(T); }
    T& Last() { return m_data.back(); }
    const T& Last() const { return m_data.back(); }
    void Pop() { m_data.pop_back(); }
    void Clear() { m_data.clear(); }
    bool IsNotEmpty() const { return !m_data.empty(); }
    bool IsEmpty() const { return m_data.empty(); }

    auto begin() { return m_data.begin(); }
    auto end() { return m_data.end(); }
    auto begin() const { return m_data.begin(); }
    auto end() const { return m_data.end(); }

    T& operator[](size_t i) { return m_data[i]; }
    const T& operator[](size_t i) const { return m_data[i]; }

private:
    std::vector<T> m_data;
};

}
