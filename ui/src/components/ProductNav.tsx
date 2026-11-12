export type ProductView = "studio" | "report" | "validation" | "mlir" | "import" | "stepper";

const items: { view: ProductView; label: string; href: string }[] = [
  { view: "studio", label: "Проект", href: "/" },
  { view: "validation", label: "Валидация", href: "/?view=validation" },
  { view: "mlir", label: "MLIR / NPZ", href: "/?view=mlir" },
  { view: "import", label: "Импорт результата", href: "/?view=import" },
  { view: "stepper", label: "Stepper", href: "/?view=stepper" },
];

export function ProductNav({ current }: { current: ProductView }) {
  return (
    <nav className="product-nav" aria-label="Разделы CacheScope">
      {items.map((item) => (
        <a
          key={item.view}
          href={item.href}
          className={item.view === current ? "active" : undefined}
          aria-current={item.view === current ? "page" : undefined}
        >
          {item.label}
        </a>
      ))}
    </nav>
  );
}
