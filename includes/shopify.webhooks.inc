<?php
/**
 * @file
 * Contains functions relating to Shopify webhooks.
 */

/**
 * Shopify webhook menu callback.
 */
function shopify_incoming_webhook() {
  $json = file_get_contents('php://input');
  $hook = str_replace('/', '_', $_SERVER['HTTP_X_SHOPIFY_TOPIC']);

  // Log webhooks if the debug option is set.
  if (variable_get('shopify_webhooks_debug', FALSE)) {
    watchdog('shopify_webhook', 'Incomming '.$hook . ':' . check_plain(var_export(json_decode($json,true), TRUE)));
  }

  //add the webhook to the queue
  shopify_shopify_webhook_queue($hook,$json);

  if(lock_acquire('shopify_webhook_queue_processing')){
    //process a single queue item - if queue is not backlogged then instant sync
    $queue = DrupalQueue::get('shopify_webhook_queued_sync', true);
    $item = $queue->claimItem();
    if($item !== false){
      if(shopify_shopify_webhook_process_queue_op((array)$item->data)){
        //item has been processed. Let's delete it
        $queue->deleteItem($item);
      }
    }

    lock_release('shopify_webhook_queue_processing');
  }

}


function shopify_shopify_webhook_findUnprocessedQueueBefore($product_id, $time=false){
  $time = ($time === false) ? time() : $time;

  $regex = '"id":'.$product_id;

  $query = <<<EOL
select *
from queue q
where q.name = 'shopify_webhook_queued_sync'
and q.data REGEXP '$regex'
and q.created < $time
order by q.created ASC
EOL;

  $result = db_query($query);
  $ids = array();
  foreach($result as $record){
    $ids[] = $record->item_id;
  }

  return $ids;
}

function shopify_shopify_webhook_process_queue_op($item){

  if($item === false){
    shopify_sync_log('Webhook queue item is false!',0);
    return;
  }

  $hook = $item['command'];
  $payload = json_decode($item['payload'],true);

  $id = (isset($payload['id'])) ? $payload['id'] : null;
  shopify_sync_log('Processing webhook %command id: %id',1, array('%command'=>$hook, '%id'=>$id));

  // Allow modules to hook into webhooks firing.
  module_invoke_all('shopify_webhook', $hook, $payload);
  module_invoke_all('shopify_webhook_' . $hook, $payload);

  return true;
}

function shopify_shopify_webhook_markQueueProcessed($ids, $status=1){

  foreach($ids as $id){

    shopify_sync_log('Removing webhook queue item_id: %id',1, array('%id'=>$id));

    //delete the queue item
    $deleted = db_delete('queue')
      ->condition('item_id', $id)
      ->execute();
  }

  return true;
}

function shopify_shopify_webhook_queue($hook,$json){
  $queue = DrupalQueue::get('shopify_webhook_queued_sync', true);

  $data = array(
    'command'=>$hook,
    'payload'=>$json
  );

  if(!$queue->createItem($data)){
    shopify_sync_log('Could not create webhook queue!',0);
  }
}

/**
 * Implements hook_shopify_webhook().
 */
function shopify_shopify_webhook($hook, $payload) {
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_fired', $hook);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_create($product) {
  shopify_shopify_webhook_products_update($product);
  if (module_exists('rules')) {
    $product_entity = shopify_product_load_single($product['id']);
    rules_invoke_event('shopify_webhook_product_created', $product_entity);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_update($collection) {
  shopify_collection_update($collection, $sync_products = TRUE);
  if (module_exists('rules')) {
    $term = shopify_collection_load($collection['id']);
    rules_invoke_event('shopify_webhook_collection_updated', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_delete($collection) {
  $term = shopify_collection_load($collection['id']);
  shopify_collection_delete($collection['id']);
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_collection_deleted', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_orders_paid($order) {
  // Update product inventory counts for purchased products.
  foreach ($order['line_items'] as $line_item) {
    if ($variant = shopify_product_load_by_variant_id($line_item['variant_id'])) {
      shopify_product_update($line_item['product_id'], $variant->variant_id, shopify_api_get_variant($variant->variant_id, array(
        'fields' => 'inventory_quantity,old_inventory_quantity',
      )));
    }
  }
  if (module_exists('rules')) {
    rules_invoke_event_by_args('shopify_webhook_order_paid', shopify_rules_parse_order_args($order));
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_shop_update($payload) {
  // Update store info on the site.
  variable_set('shopify_store_info', $payload);
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_create($collection) {
  shopify_collection_create($collection, $sync_products = TRUE);
  if (module_exists('rules')) {
    $term = shopify_collection_load($collection['id']);
    rules_invoke_event('shopify_webhook_collection_created', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_update($product) {
  $product_entity = shopify_product_update($product['id'], 0, $product);
  shopify_sync_alterProductFieldsFromVariant($product, $product_entity);
  $product_entity->save();

  foreach ($product['variants'] as $v) {
    if($v['id'] != 0){
      $variant = shopify_product_update($product['id'], $v['id'], $v);
      $variant->save();
    }
  }

  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_product_updated', $product_entity);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_delete($product) {
  $product_entity = shopify_product_load_single($product['id']);
  shopify_delete_by_product_id($product['id']);
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_product_deleted', $product_entity);
  }
}
