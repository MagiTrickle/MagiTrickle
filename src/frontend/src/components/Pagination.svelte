<script lang="ts">
  import { t } from "../data/locale.svelte";
  import Button from "./ui/Button.svelte";

  import { ChevronLeft, ChevronRight } from "./ui/icons";

  type Props = {
    totalItems: number;
    pageSize: number;
    currentPage: number;
    onPageChange?: (page: number) => void;
  };

  let { totalItems, pageSize, currentPage = $bindable(), onPageChange }: Props = $props();

  let totalPages = $derived(Math.ceil(totalItems / pageSize));
  let startItem = $derived((currentPage - 1) * pageSize + 1);
  let endItem = $derived(Math.min(currentPage * pageSize, totalItems));

  function goPrev() {
    if (currentPage > 1) {
      currentPage--;
      onPageChange?.(currentPage);
    }
  }

  function goNext() {
    if (currentPage < totalPages) {
      currentPage++;
      onPageChange?.(currentPage);
    }
  }
</script>

{#if totalPages > 1}
  <div class="pagination">
    <div class="info">
      {t("Showing")} <span class="highlight">{startItem}-{endItem}</span>
      {t("of")} <span class="highlight">{totalItems}</span>
    </div>
    <div class="controls">
      <Button small onclick={goPrev} inactive={currentPage === 1} title={t("Previous Page")}>
        <ChevronLeft size={20} />
      </Button>

      <span class="page-number">
        {currentPage} / {totalPages}
      </span>

      <Button small onclick={goNext} inactive={currentPage === totalPages} title={t("Next Page")}>
        <ChevronRight size={20} />
      </Button>
    </div>
  </div>
{/if}

<style>
  .pagination {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0.5rem;
    background-color: var(--bg-light);
    border-top: 1px solid var(--bg-light-extra);
    border-radius: 0 0 0.5rem 0.5rem;
    font-size: 0.9rem;
    color: var(--text-2);
  }

  .info {
    margin-left: 0.5rem;
  }

  .highlight {
    color: var(--text);
    font-weight: 500;
  }

  .controls {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .page-number {
    min-width: 3rem;
    text-align: center;
    font-variant-numeric: tabular-nums;
  }
</style>
