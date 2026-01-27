<script lang="ts">
  const ICON_PATHS = {
    Telegram: `<path d="M9.78 18.65l.28-4.23l7.68-6.92c.34-.31-.07-.46-.52-.19L7.74 13.3L3.64 12c-.88-.25-.89-.86.2-1.3l15.97-6.16c.73-.33 1.43.18 1.15 1.3l-2.72 12.81c-.19.91-.74 1.13-1.5.71L12.6 16.3l-1.99 1.93c-.23.23-.42.42-.83.42z"/>`,
  } as const;

  type Props = {
    name: keyof typeof ICON_PATHS;
    size?: number;
    fill?: string;
    x?: number;
    y?: number;
    [key: string]: any;
  };

  let { name, size = 24, fill = "currentColor", x = 0, y = 0, ...rest }: Props = $props();
</script>

<svg
  xmlns="http://www.w3.org/2000/svg"
  width={`${size}px`}
  height={`${size}px`}
  viewBox={`${x} ${y} ${size} ${size}`}
  {fill}
  {...rest}
>
  {@html ICON_PATHS[name]}
</svg>
