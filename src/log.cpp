
/*
fmt::format_context::iterator
fmt::formatter<Rect>::format(const Rect &box, fmt::format_context &ctx) const {
  std::stringstream str;
  str << box;
  return formatter<string_view>::format(str.str(), ctx);
}*/
