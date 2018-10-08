#pragma once

#include "common/wrappers/iterator_range.h"

#include "compiler/common.h"

/*** Id ***/
template<class IdData>
class Id {
  IdData *ptr;
public:
  Id();
  explicit Id(IdData *ptr);
  Id(const Id &id);
  Id &operator=(const Id &id);
  IdData &operator*() const;
  explicit operator bool() const {
    return ptr != nullptr;
  }
  void clear();

  IdData *operator->() const;

  bool operator==(const Id<IdData> &other) const {
    return (unsigned long)ptr == (unsigned long)other.ptr;
  }
  bool operator!=(const Id<IdData> &other) const {
    return (unsigned long)ptr != (unsigned long)other.ptr;
  }

  string &str();
};

/*** [get|set]_index ***/
template<class T>
int get_index(const Id<T> &i);

template<class T>
void set_index(Id<T> &d, int index);

/*** IdMapBase ***/
struct IdMapBase {
  virtual void renumerate_ids(const vector<int> &new_ids) = 0;
  virtual void update_size(int new_max_id) = 0;
  virtual void clear() = 0;

  virtual ~IdMapBase() {}
};

/*** IdMap ***/
template<class DataType>
struct IdMap : public IdMapBase {
  DataType def_val;
  vector<DataType> data;

  typedef typename vector<DataType>::iterator iterator;
  IdMap();
  explicit IdMap(int size, DataType val = DataType());
  template<class IndexType>
  DataType &operator[](const IndexType &i);
  template<class IndexType>
  const DataType &operator[](const IndexType &i) const;

  iterator begin();
  iterator end();
  void clear();

  void renumerate_ids(const vector<int> &new_ids);

  void update_size(int n);
};

/*** IdGen ***/
template<class IdType>
struct IdGen {
  typedef typename IdMap<IdType>::iterator iterator;

  vector<IdMapBase *> id_maps;
  IdMap<IdType> ids;
  int n;

  IdGen();
  void add_id_map(IdMapBase *to_add);
  void remove_id_map(IdMapBase *to_remove);

  int init_id(IdType *id);
  int size();
  iterator begin();
  iterator end();
  void clear();

  template<class IndexType>
  void delete_ids(const vector<IndexType> &to_del);
};

#include "graph.hpp"
