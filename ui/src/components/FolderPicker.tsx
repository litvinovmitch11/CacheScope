interface Props {
    onFiles: (files: FileList) => void;
    busy: boolean;
  }
  
  export function FolderPicker({ onFiles, busy }: Props) {
    return (
      <label className="picker">
        <span>{busy ? "Читаю…" : "Выбрать папку output/<experiment>/"}</span>
        <input
          type="file"
          // webkitdirectory нет в типах React, ставим атрибут вручную
          ref={(el) => el?.setAttribute("webkitdirectory", "")}
          onChange={(e) => {
            const f = e.target.files;
            if (f && f.length > 0) onFiles(f);
          }}
        />
      </label>
    );
  }
