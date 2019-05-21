
namespace kg
{
namespace io
{

// ======================================================================
// FileAdios2

inline FileAdios2::FileAdios2(adios2::Engine engine, adios2::IO io)
  : engine_{engine}, io_{io}
{}

inline void FileAdios2::close()
{
  engine_.Close();
}

inline void FileAdios2::performPuts()
{
  engine_.PerformPuts();
}

inline void FileAdios2::performGets()
{
  engine_.PerformGets();
}

template <typename T>
inline void FileAdios2::putVariable(const std::string& name, const T* data,
                                    const Mode launch, const Dims& shape,
                                    const Box<Dims>& selection,
                                    const Box<Dims>& memory_selection)
{
  auto v = io_.InquireVariable<T>(name);
  if (!v) {
    v = io_.DefineVariable<T>(name, shape);
  }
  if (!selection.first.empty()) {
    v.SetSelection(selection);
  }
  if (!memory_selection.first.empty()) {
    v.SetMemorySelection(memory_selection);
  }
  engine_.Put(v, data, launch);
}

template <typename T>
inline void FileAdios2::getVariable(const std::string& name, T* data,
                                    const Mode launch,
                                    const Box<Dims>& selection,
                                    const Box<Dims>& memory_selection)
{
  auto& io = const_cast<adios2::IO&>(io_); // FIXME
  auto v = io.InquireVariable<T>(name);
  if (!selection.first.empty()) {
    v.SetSelection(selection);
  }
  if (!memory_selection.first.empty()) {
    v.SetMemorySelection(memory_selection);
  }
  engine_.Get(v, data, launch);
}

struct FileAdios2::PutVariable
{
  PutVariable(FileAdios2& self, const std::string& name, Mode launch,
              const Dims& shape, const Box<Dims>& selection,
              const Box<Dims>& memory_selection)
    : self{self},
      name{name},
      launch{launch},
      shape{shape},
      selection{selection},
      memory_selection{memory_selection}
  {}

  template <typename T>
  void operator()(const T* data)
  {
    self.putVariable(name, data, launch, shape, selection, memory_selection);
  }

  FileAdios2& self;
  const std::string& name;
  Mode launch;
  const Dims& shape;
  const Box<Dims>& selection;
  const Box<Dims>& memory_selection;
};

inline void FileAdios2::putVariable(const std::string& name,
                                    TypeConstPointer data, Mode launch,
                                    const Dims& shape,
                                    const Box<Dims>& selection,
                                    const Box<Dims>& memory_selection)
{
  mpark::visit(
    PutVariable{*this, name, launch, shape, selection, memory_selection}, data);
}

struct FileAdios2::GetVariable
{
  GetVariable(FileAdios2& self, const std::string& name, Mode launch,
              const Box<Dims>& selection, const Box<Dims>& memory_selection)
    : self{self},
      name{name},
      launch{launch},
      selection{selection},
      memory_selection{memory_selection}
  {}

  template <typename T>
  void operator()(T* data)
  {
    self.getVariable(name, data, launch, selection, memory_selection);
  }

  FileAdios2& self;
  const std::string& name;
  Mode launch;
  const Box<Dims>& selection;
  const Box<Dims>& memory_selection;
};

inline void FileAdios2::getVariable(const std::string& name, TypePointer data,
                                    Mode launch, const Box<Dims>& selection,
                                    const Box<Dims>& memory_selection)
{
  mpark::visit(GetVariable{*this, name, launch, selection, memory_selection},
               data);
}

inline Dims FileAdios2::shapeVariable(const std::string& name) const
{
  auto& io = const_cast<adios2::IO&>(io_); // FIXME
  auto type = io.VariableType(name);

  if (0) {
  }
#define make_case(T)                                                           \
  else if (type == adios2::GetType<T>())                                       \
  {                                                                            \
    auto v = io.InquireVariable<T>(name);                                      \
    return v.Shape();                                                          \
  }
  ADIOS2_FOREACH_STDTYPE_1ARG(make_case)
#undef make_case
  assert(0);
}

template <typename T>
inline void FileAdios2::putAttribute(const std::string& name, const T* data,
                                     size_t size)
{
  // if (mpiRank() != 0) { // FIXME, should we do this?
  //   return;
  // }
  auto attr = io_.InquireAttribute<T>(name);
  if (attr) {
    mprintf("attr '%s' already exists -- ignoring it!\n", name.c_str());
  } else {
    // FIXME? we're never using "single value", just an array of size 1
    io_.DefineAttribute<T>(name, data, size);
  }
}

template <typename T>
inline void FileAdios2::getAttribute(const std::string& name, T* data)
{
  auto attr = io_.InquireAttribute<T>(name);
  auto vec = attr.Data();
  std::copy(vec.begin(), vec.end(), data);
}

struct FileAdios2::GetAttribute
{
  GetAttribute(FileAdios2& self, const std::string& name)
    : self{self}, name{name}
  {}

  template <typename T>
  void operator()(T* data)
  {
    self.getAttribute(name, data);
  }

  FileAdios2& self;
  const std::string& name;
};

inline void FileAdios2::getAttribute(const std::string& name, TypePointer data)
{
  mpark::visit(GetAttribute{*this, name}, data);
}

struct FileAdios2::PutAttribute
{
  PutAttribute(FileAdios2& self, const std::string& name, size_t size)
    : self{self}, name{name}, size{size}
  {}

  template <typename T>
  void operator()(T* data)
  {
    self.putAttribute(name, data, size);
  }

  FileAdios2& self;
  const std::string& name;
  size_t size;
};

inline void FileAdios2::putAttribute(const std::string& name, TypeConstPointer data, size_t size)
{
  mpark::visit(PutAttribute{*this, name, size}, data);
}

inline Dims FileAdios2::shapeAttribute(const std::string& name) const
{
  auto& io = const_cast<adios2::IO&>(io_); // FIXME
  auto type = io.AttributeType(name);

  if (0) {
  }
#define make_case(T)                                                           \
  else if (type == adios2::GetType<T>())                                       \
  {                                                                            \
    auto a = io.InquireAttribute<T>(name);                                     \
    auto vec = a.Data();                                                       \
    /* adios2 FIXME, no way to distinguish single value */                     \
    return {vec.size()};                                                       \
  }
  ADIOS2_FOREACH_ATTRIBUTE_STDTYPE_1ARG(make_case)
#undef make_case
  assert(0);
}

} // namespace io
} // namespace kg
