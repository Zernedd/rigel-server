using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class PageData
{
    [JsonPropertyName("total_items")]
    public int TotalItems { get; set; }
    
    [JsonPropertyName("item_count")]
    public int ItemCount { get; set; }
    
    [JsonPropertyName("page_size")]
    public int PageSize { get; set; }
    
    [JsonPropertyName("page")]
    public int Page { get; set; }

    [JsonPropertyName("pages")]
    public int Pages { get; set; }
}

public abstract class PagedResponse<T>
{
    [JsonPropertyName("page")]
    public PageData Page { get; set; }

    [JsonPropertyName("items")]
    public List<T> Items { get; set; }

    public static PagedResponse<T> Create<TResponse>(List<T> items, int pageSize, int defaultPage = 1)
        where TResponse : PagedResponse<T>
    {
        if (items == null)
            throw new ArgumentNullException(nameof(items));
        if (pageSize <= 0)
            throw new ArgumentException("Page size must be greater than 0", nameof(pageSize));
        if (defaultPage <= 0)
            throw new ArgumentException("Page number must be greater than 0", nameof(defaultPage));

        var totalItems = items.Count;
        var totalPages = (int)Math.Ceiling(totalItems / (double)pageSize);

        defaultPage = Math.Min(defaultPage, Math.Max(totalPages, 1));
        var skip = (defaultPage - 1) * pageSize;
        var pagedItems = items
            .Skip(skip)
            .Take(pageSize)
            .ToList();
        
        var activator = Activator.CreateInstance<TResponse>();
        activator.Items = pagedItems;
        activator.Page = new PageData
        {
            TotalItems = totalItems,
            ItemCount = pagedItems.Count,
            PageSize = pageSize,
            Page = defaultPage,
            Pages = totalPages
        };

        return activator;
    }
}
