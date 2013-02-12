/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    hilbert_basis.cpp

Abstract:

    Basic Hilbert Basis computation.

Author:

    Nikolaj Bjorner (nbjorner) 2013-02-09.

Revision History:

--*/

#include "hilbert_basis.h"
#include "heap.h"
#include "map.h"

typedef u_map<unsigned> offset_refs_t;

template<typename Value>
class rational_map : public map<rational, Value, rational::hash_proc, rational::eq_proc> {};

class rational_lt {
    vector<rational> & m_values;
public:
    rational_lt(vector<rational> & values):
        m_values(values) {
    }
    bool operator()(int v1, int v2) const {
        return m_values[v1] < m_values[v2];
    }
};


class hilbert_basis::rational_heap {
    vector<numeral>          m_u2r;     // [index |-> weight]
    rational_map<unsigned>   m_r2u;     // [weight |-> index]
    rational_lt              m_lt;      // less_than on indices
    heap<rational_lt>        m_heap;    // binary heap over weights
public:

    rational_heap(): m_lt(m_u2r), m_heap(10, m_lt) {}

    vector<numeral>& u2r() { return m_u2r; }

    void insert(unsigned v) {
        m_heap.insert(v);
    }

    void reset() {
        m_u2r.reset();
        m_r2u.reset();
        m_heap.reset();
    }

    bool is_declared(numeral const& r, unsigned& val) const {
        return m_r2u.find(r, val);
    }

    unsigned declare(numeral const& r) {
        SASSERT(!m_r2u.contains(r));
        unsigned val = m_u2r.size();
        m_u2r.push_back(r);
        m_r2u.insert(r, val);
        m_heap.set_bounds(val+1);
        return val;
    }

    void find_le(unsigned val, int_vector & result) {
        m_heap.find_le(val, result);
    }

    void find_le(numeral const& r, int_vector& result) {
        find_le(m_r2u.find(r), result);
    }
};

class hilbert_basis::weight_map {
    rational_heap            m_heap;         
    vector<unsigned_vector>  m_offsets;      // [index |-> offset-list]
    int_vector               m_le;           // recycled set of indices with lesser weights
    
    unsigned get_value(numeral const& w) {
        unsigned val;
        if (!m_heap.is_declared(w, val)) {
            val = m_heap.declare(w);
            SASSERT(val == m_offsets.size());
            if (w.is_nonneg()) {
                m_heap.insert(val);
            }
            m_offsets.push_back(unsigned_vector());
        }
        return val;
    }
public:
    weight_map() {}
    
    void insert(offset_t idx, numeral const& w) {
        unsigned val = get_value(w);
        m_offsets[val].push_back(idx.m_offset);
    }

    void remove(offset_t idx, numeral const& w) {
        unsigned val = get_value(w);
        m_offsets[val].erase(idx.m_offset);
    }

    void reset() {
        m_offsets.reset();
        m_heap.reset();
        m_le.reset();
    }
    
    bool init_find(offset_refs_t& refs, numeral const& w, offset_t idx, offset_t& found_idx, unsigned& cost) {
        //std::cout << "init find: " << w << "\n";
        m_le.reset();
        unsigned val = get_value(w);
        // for positive values, the weights should be less or equal.
        // for non-positive values, the weights have to be the same.
        if (w.is_pos()) {
            m_heap.find_le(val, m_le);
        }
        else {
            m_le.push_back(val);
        }
        bool found = false;
        for (unsigned i = 0; i < m_le.size(); ++i) {
            if (m_heap.u2r()[m_le[i]].is_zero() && w.is_pos()) {
                continue;
            } 
            //std::cout << "insert init find: " << m_weights[m_le[i]] << "\n";
            unsigned_vector const& offsets = m_offsets[m_le[i]];
            for (unsigned j = 0; j < offsets.size(); ++j) {
                unsigned offs = offsets[j];
                ++cost;
                if (offs != idx.m_offset) {
                    refs.insert(offs, 0);
                    found_idx = offset_t(offs);
                    found = true;
                }
            }
        }
        return found;
    }
    
    bool update_find(offset_refs_t& refs, unsigned round, numeral const& w, 
                     offset_t idx, offset_t& found_idx, unsigned& cost) {
        //std::cout << "update find: " << w << "\n";
        m_le.reset();
        m_heap.find_le(w, m_le);
        bool found = false;
        unsigned vl;
        for (unsigned i = 0; i < m_le.size(); ++i) {
            //std::cout << "insert update find: " << m_weights[m_le[i]] << "\n";
            unsigned_vector const& offsets = m_offsets[m_le[i]];
            for (unsigned j = 0; j < offsets.size(); ++j) {
                unsigned offs = offsets[j];
                ++cost;
                if (offs != idx.m_offset && refs.find(offs, vl) && vl == round) {
                    refs.insert(offs, round + 1);
                    found_idx = offset_t(offs);
                    found = true;
                }
            }
        }
        return found;
    }    
};


class hilbert_basis::index {
    // for each index, a heap of weights.
    // for each weight a list of offsets

    struct stats {
        unsigned m_num_comparisons;
        unsigned m_num_find;
        unsigned m_num_insert;
        stats() { reset(); }
        void reset() { memset(this, 0, sizeof(*this)); }
    };    
    ptr_vector<weight_map> m_values;
    weight_map         m_weight;
    offset_refs_t      m_refs;
    stats              m_stats;

public:

    ~index() {
        for (unsigned i = 0; i < m_values.size(); ++i) {
            dealloc(m_values[i]);
        }
    }

    void init(unsigned num_vars) {
        if (m_values.empty()) {
            for (unsigned i = 0; i < num_vars; ++i) {
                m_values.push_back(alloc(weight_map));
            }
        }
        SASSERT(m_values.size() == num_vars);
    }
    
    void insert(offset_t idx, values vs, numeral const& weight) {
        ++m_stats.m_num_insert;
        for (unsigned i = 0; i < m_values.size(); ++i) {
            m_values[i]->insert(idx, vs[i]);
        }
        m_weight.insert(idx, weight);
    }

    void remove(offset_t idx, values vs, numeral const& weight) {
        for (unsigned i = 0; i < m_values.size(); ++i) {
            m_values[i]->remove(idx, vs[i]);
        }
        m_weight.remove(idx, weight);
    }

    bool find(values vs, numeral const& weight, offset_t idx, offset_t& found_idx) {
        ++m_stats.m_num_find;
        bool found = m_weight.init_find(m_refs, weight, idx, found_idx, m_stats.m_num_comparisons);
        for (unsigned i = 0; found && i < m_values.size(); ++i) {
            found = m_values[i]->update_find(m_refs, i, vs[i], idx, found_idx, m_stats.m_num_comparisons);
        }        
        m_refs.reset();
        return found;
    }

    void reset() {
        for (unsigned i = 0; i < m_values.size(); ++i) {
            m_values[i]->reset();
        }
        m_weight.reset();
        m_refs.reset();
    }

    void collect_statistics(statistics& st) const {
        st.update("hb.index.num_comparisons", m_stats.m_num_comparisons);
        st.update("hb.index.num_find", m_stats.m_num_find);
        st.update("hb.index.num_insert", m_stats.m_num_insert);
    }

    void reset_statistics() {
        m_stats.reset();
    }

#if 0
    // remains of a simpler index strucure:
    if (eval(idx).is_zero()) {
        for (unsigned i = 0; i < m_zero.size(); ++i) {
            if (is_subsumed(idx, m_zero[i])) {
                ++m_stats.m_num_subsumptions;
                return true;
            }
        }
        return false;
    }
    for (unsigned i = 0; i < m_active.size(); ++i) {
        if (is_subsumed(idx, m_active[i])) {
            ++m_stats.m_num_subsumptions;
            return true;
        }
    }
    passive::iterator it = m_passive->begin();
    passive::iterator end = m_passive->end();

    for (; it != end; ++it) {
        if (is_subsumed(idx, *it)) {
            ++m_stats.m_num_subsumptions;
            return true;
        }
    }    
#endif

};

/**
   \brief priority queue for passive list.
*/

class hilbert_basis::passive {
    hilbert_basis&        hb;
    svector<offset_t>     m_passive;
    vector<numeral>       m_weights;
    unsigned_vector       m_free_list;
    rational_lt           m_lt;      // less_than on indices
    heap<rational_lt>     m_heap;    // binary heap over weights

    numeral get_weight(offset_t idx) {
        numeral w(0);
        unsigned nv = hb.get_num_vars();
        for (unsigned i = 0; i < nv; ++i) {
            w += hb.vec(idx)[i];
        }
        return w;
    }
    
public:
    
    passive(hilbert_basis& hb): 
        hb(hb) ,
        m_lt(m_weights),
        m_heap(10, m_lt)
    {}

    void reset() {
        m_heap.reset();
        m_free_list.reset();
        m_weights.reset();
        m_passive.reset();
    }
    
    bool empty() const {
        return m_heap.empty();
    }

    offset_t pop() {
        SASSERT(!empty());
        unsigned val = static_cast<unsigned>(m_heap.erase_min());        
        offset_t result = m_passive[val];
        m_free_list.push_back(val);
        m_passive[val] = mk_invalid_offset();
        return result;
    }
    
    void insert(offset_t idx) {
        unsigned v;
        if (m_free_list.empty()) {
            v = m_passive.size();
            m_passive.push_back(idx);
            m_weights.push_back(get_weight(idx));
            m_heap.set_bounds(v+1);
        }
        else {
            v = m_free_list.back();
            m_free_list.pop_back();
            m_passive[v] = idx;
            m_weights[v] = get_weight(idx);
        }
        m_heap.insert(v);
    }

    class iterator {
        passive& p;
        unsigned m_idx;
        void fwd() {
            while (m_idx < p.m_passive.size() && 
                   is_invalid_offset(p.m_passive[m_idx])) {
                ++m_idx;
            }
        }
    public:
        iterator(passive& p, unsigned i): p(p), m_idx(i) { fwd(); }
        offset_t operator*() const { return p.m_passive[m_idx]; }
        iterator& operator++() { ++m_idx; fwd(); return *this; }
        iterator operator++(int) { iterator tmp = *this; ++*this; return tmp; }
        bool operator==(iterator const& it) const {return m_idx == it.m_idx; }
        bool operator!=(iterator const& it) const {return m_idx != it.m_idx; }

    };

    iterator begin() {
        return iterator(*this, 0);
    }

    iterator end() {
        return iterator(*this, m_passive.size());
    }
};

hilbert_basis::hilbert_basis(): 
    m_cancel(false) 
{
    m_index = alloc(index);
    m_passive = alloc(passive, *this);
}

hilbert_basis::~hilbert_basis() {
    dealloc(m_index);
    dealloc(m_passive);
}

hilbert_basis::offset_t hilbert_basis::mk_invalid_offset() {
    return offset_t(UINT_MAX);
}

bool hilbert_basis::is_invalid_offset(offset_t offs) {
    return offs.m_offset == UINT_MAX;
}

void hilbert_basis::reset() {
    m_ineqs.reset();
    m_basis.reset();
    m_store.reset();
    m_free_list.reset();
    m_eval.reset();
    m_active.reset();
    m_passive->reset();
    m_zero.reset();
    m_index->reset();
    m_cancel = false;
}

void hilbert_basis::collect_statistics(statistics& st) const {
    st.update("hb.num_subsumptions", m_stats.m_num_subsumptions);
    st.update("hb.num_resolves", m_stats.m_num_resolves);
    m_index->collect_statistics(st);
}

void hilbert_basis::reset_statistics() {
    m_stats.reset();
    m_index->reset_statistics();
}

void hilbert_basis::add_ge(num_vector const& v) {
    SASSERT(m_ineqs.empty() || v.size() == get_num_vars());
    if (m_ineqs.empty()) {
        m_index->init(v.size());
    }
    m_ineqs.push_back(v);
}

void hilbert_basis::add_le(num_vector const& v) {
    num_vector w(v);
    for (unsigned i = 0; i < w.size(); ++i) {
        w[i].neg();
    }
    add_ge(w);
}

void hilbert_basis::add_eq(num_vector const& v) {
    add_le(v);
    add_ge(v);
}

unsigned hilbert_basis::get_num_vars() const {
    if (m_ineqs.empty()) {
        return 0;
    }
    else {
        return m_ineqs.back().size();
    }
}

hilbert_basis::values hilbert_basis::vec(offset_t offs) const {
    return m_store.c_ptr() + offs.m_offset;
}

hilbert_basis::values_ref hilbert_basis::vec(offset_t offs) {
    return m_store.c_ptr() + offs.m_offset;
}

void hilbert_basis::init_basis() {
    m_basis.reset();
    m_store.reset();
    m_eval.reset();
    m_free_list.reset();
    unsigned num_vars = get_num_vars();
    for (unsigned i = 0; i < num_vars; ++i) {
        num_vector w(num_vars, numeral(0));
        w[i] = numeral(1);
        offset_t idx = alloc_vector();
        set_value(idx, w.c_ptr());
        m_basis.push_back(idx);
    }
}

lbool hilbert_basis::saturate() {
    init_basis();    
    for (unsigned i = 0; !m_cancel && i < m_ineqs.size(); ++i) {
        lbool r = saturate(m_ineqs[i]);
        if (r != l_true) {
            return r;
        }
    }
    if (m_cancel) {
        return l_undef;
    }
    return l_true;
}

lbool hilbert_basis::saturate(num_vector const& ineq) {
    m_active.reset();
    m_passive->reset();
    m_zero.reset();
    m_index->reset();
    TRACE("hilbert_basis", display_ineq(tout, ineq););
    bool has_non_negative = false;
    iterator it = begin();
    for (; it != end(); ++it) {
        numeral n = eval(vec(*it), ineq);
        eval(*it) = n;
        add_goal(*it);
        if (n.is_nonneg()) {
            has_non_negative = true;
        }
    }
    TRACE("hilbert_basis", display(tout););
    if (!has_non_negative) {
        return l_false;
    }
    // resolve passive into active
    while (!m_passive->empty()) {
        if (m_cancel) {
            return l_undef;
        }
        offset_t idx = m_passive->pop();
        TRACE("hilbert_basis", display(tout););
        if (is_subsumed(idx)) {
            recycle(idx);
            continue;
        }
        for (unsigned i = 0; !m_cancel && i < m_active.size(); ++i) {
            if (get_sign(idx) != get_sign(m_active[i])) {
                offset_t j = alloc_vector();
                resolve(idx, m_active[i], j);
                add_goal(j);
            }
        }
        m_active.push_back(idx);
    }
    // Move positive from active and zeros to new basis.
    m_basis.reset();
    m_basis.append(m_zero);
    for (unsigned i = 0; i < m_active.size(); ++i) {
        offset_t idx = m_active[i];
        if (eval(idx).is_pos()) {
            m_basis.push_back(idx);
        }
        else {
            m_free_list.push_back(idx);
        }
    }
    m_active.reset();
    m_passive->reset();
    m_zero.reset();
    TRACE("hilbert_basis", display(tout););
    return l_true;
}


void hilbert_basis::set_value(offset_t offs, values v) {
    unsigned nv = get_num_vars();
    for (unsigned i = 0; i < nv; ++i) {
        m_store[offs.m_offset+i] = v[i];
    }
}

void hilbert_basis::recycle(offset_t idx) {
    m_index->remove(idx, vec(idx), eval(idx));
    m_free_list.push_back(idx);
}

void hilbert_basis::resolve(offset_t i, offset_t j, offset_t r) {
    ++m_stats.m_num_resolves;
    values v = vec(i);
    values w = vec(j);
    values_ref u = vec(r);
    unsigned nv = get_num_vars();
    for (unsigned k = 0; k < nv; ++k) {
        u[k] = v[k] + w[k];
    }
    eval(r) = eval(i) + eval(j);
    TRACE("hilbert_basis_verbose",
          display(tout, i); 
          display(tout, j); 
          display(tout, r); 
          );

}

hilbert_basis::offset_t hilbert_basis::alloc_vector() {
    if (m_free_list.empty()) {
        unsigned num_vars = get_num_vars();
        unsigned idx =  m_store.size();
        m_store.resize(idx + get_num_vars());
        m_eval.push_back(numeral(0));
        return offset_t(idx);
    }
    else {
        offset_t result = m_free_list.back();
        m_free_list.pop_back();
        return result;
    }
}


void hilbert_basis::add_goal(offset_t idx) {
    m_index->insert(idx, vec(idx), eval(idx));
    if (eval(idx).is_zero()) {
        if (!is_subsumed(idx)) {
            m_zero.push_back(idx);
        }
    }
    else {
        m_passive->insert(idx);
    }
}

bool hilbert_basis::is_subsumed(offset_t idx)  {

    offset_t found_idx;
    if (m_index->find(vec(idx), eval(idx), idx, found_idx)) {        
        TRACE("hilbert_basis",  
           display(tout, idx);
           tout << " <= \n";
           display(tout, found_idx);
           tout << "\n";);
        ++m_stats.m_num_subsumptions;
        return true;
    }
    return false;
}

/**
   Vector v is subsumed by vector w if

      v[i] >= w[i] for each index i.

      a*v >= a*w  for the evaluation of vectors with respect to a.

      a*v < 0 => a*v = a*w


   Justification:
   
      let u := v - w, then
         
      u[i] >= 0  for each index i

      a*u = a*(v-w) >= 0
     
      So v = u + w, where a*u >= 0, a*w >= 0.

      If a*v >= a*w >= 0 then v and w are linear 
      solutions of e_i, and also v-w is a solution.

      If a*v = a*w < 0, then a*(v-w) = 0, so v can be obtained from w + (v - w).
      
 */

bool hilbert_basis::is_subsumed(offset_t i, offset_t j) const {
    values v = vec(i);
    values w = vec(j);
    numeral const& n = eval(i);
    numeral const& m = eval(j);
    bool r = 
        i.m_offset != j.m_offset &&         
        n >= m && (!m.is_neg() || n == m) &&
        is_geq(v, w);
    CTRACE("hilbert_basis", r, 
           display(tout, i);
           tout << " <= \n";
           display(tout, j);
           tout << "\n";);
    return r;
}

bool hilbert_basis::is_geq(values v, values w) const {
    unsigned nv = get_num_vars();
    for (unsigned i = 0; i < nv; ++i) {
        if (v[i] < w[i]) {
            return false;
        }
    }
    return true;
}

hilbert_basis::sign_t hilbert_basis::get_sign(offset_t idx) const {
    if (eval(idx).is_pos()) {
        return pos;
    }
    if (eval(idx).is_neg()) {
        return neg;
    }
    return zero;
}

hilbert_basis::numeral hilbert_basis::eval(values val, num_vector const& ineq) const {
    numeral result(0);
    unsigned num_vars = get_num_vars();
    for (unsigned i = 0; i < num_vars; ++i) {
        result += val[i]*ineq[i];
    }
    return result;
}

void hilbert_basis::display(std::ostream& out) const {
    unsigned nv = get_num_vars();
    out << "inequalities:\n";
    for (unsigned i = 0; i < m_ineqs.size(); ++i) {
        display_ineq(out, m_ineqs[i]);
    }
    if (!m_basis.empty()) {
        out << "basis:\n";
        for (iterator it = begin(); it != end(); ++it) {
            display(out, *it);
        }
    }
    if (!m_active.empty()) {
        out << "active:\n";
        for (unsigned i = 0; i < m_active.size(); ++i) {
            display(out, m_active[i]);
        }
    }
    if (!m_passive->empty()) {
        passive::iterator it = m_passive->begin();
        passive::iterator end = m_passive->end();
        out << "passive:\n";
        for (; it != end; ++it) {
            display(out, *it);
        }
    }
    if (!m_zero.empty()) {
        out << "zero:\n";
        for (unsigned i = 0; i < m_zero.size(); ++i) {
            display(out, m_zero[i]);
        }
    }

}

void hilbert_basis::display(std::ostream& out, offset_t o) const {
    display(out, vec(o));
    out << " -> " << eval(o) << "\n";    
}

void hilbert_basis::display(std::ostream& out, values v) const {
    unsigned nv = get_num_vars();
    for (unsigned j = 0; j < nv; ++j) {            
        out << v[j] << " ";
    }
}

void hilbert_basis::display_ineq(std::ostream& out, num_vector const& v) const {
    unsigned nv = get_num_vars();
    for (unsigned j = 0; j < nv; ++j) {
        if (!v[j].is_zero()) {
            if (j > 0) {
                if (v[j].is_pos()) {
                    out << " + ";
                }
                else {
                    out << " - ";
                }
            }
            else if (j == 0 && v[0].is_neg()) {
                out << "-";
            }
            if (!v[j].is_one() && !v[j].is_minus_one()) {
                out << abs(v[j]) << "*";
            }
            out << "x" << j;
        }
    }
    out << " >= 0\n";
}

void hilbert_sl_basis::add_le(num_vector const& v, numeral bound) {
    num_vector w;
    w.push_back(-bound);
    w.append(v);
    m_basis.add_le(w);
}

void hilbert_isl_basis::add_le(num_vector const& v, numeral bound) {
    unsigned sz = v.size();
    num_vector w;
    for (unsigned i = 0; i < sz; ++i) {
        w.push_back(v[i]);
        w.push_back(-v[i]);
    }
    w.push_back(-bound);
    w.push_back(bound);
    m_basis.add_le(w);
}
